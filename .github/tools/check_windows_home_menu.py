"""Drive the real first-run CLI through a native Windows console, not a pipe."""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

if os.name != 'nt':
    raise SystemExit('This check requires Windows')

kernel = c.WinDLL('kernel32', use_last_error=True)
INVALID_HANDLE_VALUE = c.c_void_p(-1).value

class Coord(c.Structure):
    _fields_ = [('X', c.c_short), ('Y', c.c_short)]

class Rect(c.Structure):
    _fields_ = [(name, c.c_short) for name in ('Left', 'Top', 'Right', 'Bottom')]

class ScreenInfo(c.Structure):
    _fields_ = [('size', Coord), ('cursor', Coord), ('attributes', w.WORD),
                ('window', Rect), ('maximum', Coord)]

class KeyChar(c.Union):
    _fields_ = [('UnicodeChar', w.WCHAR), ('AsciiChar', c.c_char)]

class KeyEvent(c.Structure):
    _fields_ = [('down', w.BOOL), ('repeat', w.WORD), ('key', w.WORD),
                ('scan', w.WORD), ('char', KeyChar), ('state', w.DWORD)]

class Event(c.Union):
    _fields_ = [('key', KeyEvent), ('padding', c.c_byte * 16)]

class InputRecord(c.Structure):
    _fields_ = [('type', w.WORD), ('event', Event)]

kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p,
                               w.DWORD, w.DWORD, w.HANDLE]
kernel.CreateFileW.restype = w.HANDLE
kernel.AttachConsole.argtypes = [w.DWORD]
kernel.AttachConsole.restype = w.BOOL
kernel.GetConsoleScreenBufferInfo.argtypes = [w.HANDLE, c.POINTER(ScreenInfo)]
kernel.GetConsoleScreenBufferInfo.restype = w.BOOL
kernel.ReadConsoleOutputCharacterW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD,
                                              Coord, c.POINTER(w.DWORD)]
kernel.ReadConsoleOutputCharacterW.restype = w.BOOL
kernel.WriteConsoleInputW.argtypes = [w.HANDLE, c.POINTER(InputRecord), w.DWORD,
                                     c.POINTER(w.DWORD)]
kernel.WriteConsoleInputW.restype = w.BOOL
kernel.CloseHandle.argtypes = [w.HANDLE]

def opened(name):
    handle = kernel.CreateFileW(name, 0xC0000000, 3, None, 3, 0, None)
    if handle == INVALID_HANDLE_VALUE:
        raise c.WinError(c.get_last_error())
    return handle

def screen(handle):
    info = ScreenInfo()
    if not kernel.GetConsoleScreenBufferInfo(handle, c.byref(info)):
        raise c.WinError(c.get_last_error())
    width = info.size.X
    rows = min(info.size.Y, max(60, info.cursor.Y + 3))
    data = c.create_unicode_buffer(width * rows + 1)
    count = w.DWORD()
    if not kernel.ReadConsoleOutputCharacterW(handle, data, width * rows, Coord(0, 0), c.byref(count)):
        raise c.WinError(c.get_last_error())
    return '\n'.join(data.value[i:i + width].rstrip() for i in range(0, count.value, width))

def input_line(handle, value):
    for char in value + '\r':
        record = InputRecord()
        record.type = 1
        record.event.key.down = True
        record.event.key.repeat = 1
        record.event.key.key = 13 if char == '\r' else ord(char.upper())
        record.event.key.char.UnicodeChar = char
        count = w.DWORD()
        if not kernel.WriteConsoleInputW(handle, c.byref(record), 1, c.byref(count)):
            raise c.WinError(c.get_last_error())
        assert count.value == 1

def check_layout(text):
    lines = text.splitlines()
    choose = next(i for i, line in enumerate(lines) if 'Choose the mcpp home:' in line)
    select = next(i for i, line in enumerate(lines) if 'Select [1/2, Enter=1]:' in line)
    assert lines[choose].index('Choose') == lines[select].index('Select') == 6, text
    assert not lines[choose + 1].strip(), text
    assert '1) D:\\mcpp' in lines[choose + 2] and '[default]' in lines[choose + 2], text
    assert '2) ' in lines[choose + 3], text
    assert not lines[select - 1].strip(), text

source = Path(os.environ['MCPP']).resolve()
with tempfile.TemporaryDirectory(prefix='mcpp-console-menu-') as temporary:
    root = Path(temporary)
    # `target` deliberately excludes portable-home resolution. Each profile is fresh.
    binary = root / 'target' / 'bin' / 'mcpp.exe'
    binary.parent.mkdir(parents=True)
    shutil.copy2(source, binary)

    def run_case(name, choice):
        profile = root / name
        profile.mkdir()
        env = os.environ.copy()
        env.pop('MCPP_HOME', None)
        env.update(USERPROFILE=str(profile), HOME=str(profile), MCPP_OFFLINE='1')
        proc = subprocess.Popen([str(binary), 'self', 'init'], env=env, cwd=root,
                                creationflags=subprocess.CREATE_NEW_CONSOLE)
        output = stdin = None
        try:
            kernel.FreeConsole()
            until = time.monotonic() + 10
            while not kernel.AttachConsole(proc.pid):
                if proc.poll() is not None or time.monotonic() >= until:
                    raise AssertionError('Could not attach the native child console')
                time.sleep(.02)
            output, stdin = opened('CONOUT$'), opened('CONIN$')
            while True:
                text = screen(output)
                if 'Select [1/2, Enter=1]:' in text:
                    break
                if proc.poll() is not None or time.monotonic() >= until:
                    raise AssertionError(text or 'No home menu appeared')
                time.sleep(.02)
            check_layout(text)
            input_line(stdin, choice)
            code = proc.wait(timeout=20)
            text = screen(output)
            print(name, 'exit', code, '\n' + text, flush=True)
            if name == 'invalid':
                assert code == 2 and '--home' in text, text
                assert not (profile / '.mcpp' / 'home.toml').exists()
            else:
                assert code in (0, 1), text  # offline initialization can lack tools
                data = subprocess.run([str(binary), 'self', 'env', '--format', 'json'],
                                      env=env, cwd=root, capture_output=True, text=True,
                                      encoding='utf-8', timeout=10, check=True)
                home = Path(json.loads(data.stdout)['data']['mcppHome'])
                expected = Path('D:/mcpp') if name == 'default' else profile / '.mcpp'
                assert os.path.normcase(str(home)) == os.path.normcase(str(expected)), data.stdout
                assert (expected / 'config.toml').is_file(), text
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
            for handle in (stdin, output):
                if handle is not None:
                    kernel.CloseHandle(handle)
            kernel.FreeConsole()

    # This check runs on an ephemeral hosted Windows runner. Refuse an existing
    # D:\mcpp rather than touching a home this test did not create.
    default = Path('D:/mcpp')
    assert Path('D:/').is_dir(), 'The native-menu lane requires its hosted D drive'
    default.mkdir()  # exclusive reservation; an existing home is a test setup error
    marker = default / '.mcpp-console-fixture'
    marker.write_text('owned by this isolated CI test', encoding='utf-8')
    try:
        run_case('invalid', '3')
        run_case('user', '2')
        run_case('default', '')
    finally:
        if marker.is_file():
            shutil.rmtree(default)
print('PASS: native Windows menu alignment, invalid input, user home, and Enter default')
