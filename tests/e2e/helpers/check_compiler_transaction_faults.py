#!/usr/bin/env python3
"""Linux x86-64 syscall-boundary controls for the actual compiler helpers."""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import time

libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.restype = ctypes.c_long
register_names = 'r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi orig_rax rip cs eflags rsp ss fs_base gs_base ds es fs gs'.split()


class Registers(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in register_names]


def ptrace(operation, pid, address=0, data=0):
    result = libc.ptrace(operation, pid, ctypes.c_void_p(address),
                         data if isinstance(data, ctypes.c_void_p) else ctypes.c_void_p(data))
    if result == -1 and ctypes.get_errno():
        raise OSError(ctypes.get_errno(), str((operation, pid)))
    return result


def string_at(pid, address):
    result = bytearray()
    for offset in range(0, 4096, 8):
        ctypes.set_errno(0)
        value = ptrace(2, pid, address + offset) & ((1 << 64) - 1)
        word = value.to_bytes(8, 'little')
        result.extend(word.split(b'\0', 1)[0])
        if b'\0' in word:
            return result.decode(errors='replace')
    return ''


def syscall_path(pid, regs):
    if regs.orig_rax == 82:  # rename
        return string_at(pid, regs.rsi)
    if regs.orig_rax in (264, 316):  # renameat, renameat2
        return string_at(pid, regs.r10)
    if regs.orig_rax == 257:  # openat
        return string_at(pid, regs.rsi)
    return ''


def trace_fault(case, boundary):
    log = (case.out / 'fault.log').open('w')
    child = os.fork()
    if child == 0:
        os.dup2(log.fileno(), 1)
        os.dup2(log.fileno(), 2)
        os.chdir(case.out)
        ptrace(0, 0)
        os.kill(os.getpid(), signal.SIGSTOP)
        os.execve(str(case.binary), [str(case.binary)], dict(case.env, MCPP_TEST_LIFETIME_DELAY='60'))
    tracees = {child}
    entering = {}
    paths = {}
    hit = None
    try:
        os.waitpid(child, 0)
        # Trace native descendants and threads without modifying product code.
        ptrace(0x4200, child, 0, 1 | 2 | 4 | 8 | 16)
        ptrace(24, child)
        limit = time.monotonic() + 25
        while time.monotonic() < limit:
            pid, status = os.waitpid(-1, os.WNOHANG | 0x40000000)
            if pid == 0:
                time.sleep(.0005)
                continue
            if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                tracees.discard(pid)
                if pid == child:
                    raise AssertionError('owner ended before fault boundary: ' + boundary)
                continue
            event = status >> 16
            stop = os.WSTOPSIG(status)
            if event in (1, 2, 3):
                spawned = ctypes.c_ulonglong()
                ptrace(0x4201, pid, 0, ctypes.cast(ctypes.byref(spawned), ctypes.c_void_p))
                tracees.add(spawned.value)
            if event == 4 and boundary == 'after-spawn':
                environment = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
                if b'MCPP_TEST_LIFETIME_ROLE=compiler' in environment:
                    hit = {'boundary': boundary, 'native_event': 'exec', 'writer': pid}
            if stop == (signal.SIGTRAP | 0x80):
                regs = Registers()
                ptrace(12, pid, 0, ctypes.cast(ctypes.byref(regs), ctypes.c_void_p))
                info=ctypes.create_string_buffer(128)
                ptrace(0x420e,pid,128,ctypes.cast(info,ctypes.c_void_p))
                entry=info.raw[0]==1
                if entry:
                    paths[pid] = syscall_path(pid, regs)
                name = Path(paths.get(pid, '')).name
                desired, side = {
                    'before-prepared': ('prepared', True), 'after-prepared': ('prepared', False),
                    'before-backup': ('baseline.bmi', True), 'after-backup': ('baseline', False),
                    'before-spawn': ('admitted', False), 'after-spawn': ('', True),
                    'bmi-published': ('published.gcm', False),
                    'before-rc': ('unit.rc', True), 'after-rc': ('unit.rc', False),
                }[boundary]
                rename = regs.orig_rax in (82, 264, 316)
                backup_open = boundary == 'before-backup' and regs.orig_rax == 257
                if name == desired and entry == side and (rename or backup_open):
                    if not entry:
                        assert regs.rax == 0, (boundary, regs.rax)
                    hit = {'boundary': boundary, 'native_event': 'syscall-entry' if entry else 'syscall-exit', 'writer': pid}
            if hit:
                # The actual state writer and owner disappear at this boundary.
                os.kill(pid, signal.SIGKILL)
                if pid != child:
                    os.kill(child, signal.SIGKILL)
                break
            ptrace(24, pid, 0, 0 if stop in (signal.SIGSTOP, signal.SIGTRAP, signal.SIGTRAP | 0x80) else stop)
        assert hit, 'fault boundary not reached: ' + boundary
    finally:
        for pid in list(tracees):
            try:
                ptrace(17, pid)
            except OSError:
                try:
                    ptrace(7, pid)
                except OSError:
                    pass
        try:
            os.kill(child, signal.SIGKILL)
        except ProcessLookupError:
            pass
        # Reap traced tasks, including threads, without retaining a stopped writer.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG | 0x40000000)
            except ChildProcessError:
                break
            if pid == 0:
                time.sleep(.005)
                continue
            if os.WIFSTOPPED(status):
                try:
                    ptrace(17, pid)
                except OSError:
                    ptrace(7, pid)
        log.close()
    return hit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('fixture', type=Path)
    args = parser.parse_args()
    assert platform.system() == 'Linux' and platform.machine() == 'x86_64'
    spec = importlib.util.spec_from_file_location('lifetime', Path(__file__).with_name('check_owned_schedule_lifetime.py'))
    lifetime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifetime)
    rows = []
    for boundary in ('before-prepared', 'after-prepared', 'before-backup', 'after-backup',
                     'before-spawn', 'after-spawn', 'bmi-published', 'before-rc', 'after-rc'):
        case = lifetime.Case(args.fixture, boundary, args.binary)
        try:
            (case.out / 'published.gcm').write_text('same BMI')
            time.sleep(.01)
            (case.out / 'source').touch()
            row = trace_fault(case, boundary)
            new = case.start(delay=30)
            case.ready(new)
            case.success(new)
            before = {name: (case.out / name).stat().st_mtime_ns for name in ('published.gcm', 'module.o')}
            calls = (case.out / 'compiler.runs').read_bytes()
            noop = case.start(delay=30)
            case.success(noop)
            assert calls == (case.out / 'compiler.runs').read_bytes(), 'recovery caused a no-op compile'
            assert before == {name: (case.out / name).stat().st_mtime_ns for name in before}
            row.update(recovery='success', unchanged_noop=True)
            rows.append(row)
            print('PASS:', boundary, 'recovery and unchanged no-op', flush=True)
        finally:
            case.close()
    (args.fixture / 'transaction-faults.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    main()
