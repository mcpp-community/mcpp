import json
import os
import shutil
import stat
from pathlib import Path
import subprocess
import tempfile

binary = Path(os.environ['MCPP']).resolve()
with tempfile.TemporaryDirectory(prefix='mcpp-home-e2e-') as tmp:
    root = Path(tmp).resolve()
    original, target, third = (root / name for name in ('original home \u0915\u0916\u0917', 'selected home \u65e5\u672c\u8a9e \U0001f642', 'third'))
    original.mkdir()
    (original / 'config.toml').write_text('# Existing home\n', encoding='utf-8')
    user = root / 'unselected user'
    user_locator = user / '.mcpp' / 'home.toml'
    user_locator.parent.mkdir(parents=True)
    user_locator.write_text('home = [\n', encoding='utf-8')
    env = os.environ | {'MCPP_HOME': str(original), 'MCPP_OFFLINE': '1',
                        'HOME': str(user), 'USERPROFILE': str(user)}

    def run(*args, expected=None, offline=True):
        childenv = env.copy()
        if not offline:
            childenv.pop('MCPP_OFFLINE', None)
        p = subprocess.run([str(binary), *args], env=childenv, cwd=root,
                           stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, encoding='utf-8', errors='replace', timeout=15)
        if expected is not None:
            assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
        return p

    # CLI argv, a relative path, spaces and Unicode survive persistence/restart.
    relative_target = str(target.relative_to(root))
    p = run('self', 'init', '--home', relative_target, expected=1)
    assert '--force' in p.stderr and not target.exists(), (p.stdout, p.stderr)
    p = run('self', 'init', '--force', '--home', relative_target)
    assert p.returncode in (0, 1), (p.stdout, p.stderr)
    assert (target / 'config.toml').is_file(), (p.stdout, p.stderr)
    assert (original / 'config.toml').is_file()
    assert (original / 'home.toml').is_file()
    data = json.loads(run('self', 'env', '--format', 'json', expected=0).stdout)['data']
    assert Path(data['mcppHome']).resolve() == target
    assert Path(data['originalHome']).resolve() == original

    # A locator in the destination is ignored, including an unavailable target.
    (target / 'home.toml').write_text(f'home = {json.dumps(third.as_posix())}\n', encoding='utf-8')
    data = json.loads(run('self', 'env', '--format', 'json', expected=0).stdout)['data']
    assert Path(data['mcppHome']).resolve() == target
    assert not third.exists()

    sentinel = target / 'registry' / 'retain-sentinel'
    sentinel.write_text('keep', encoding='utf-8')
    run('self', 'init', '--force', '--home', str(target))
    assert sentinel.is_file(), 'explicit force-home deleted existing data'

    # A mirror change must succeed with no usable network and no bootstrap.
    env.update(HTTPS_PROXY='http://127.0.0.1:9', HTTP_PROXY='http://127.0.0.1:9',
               ALL_PROXY='http://127.0.0.1:9', NO_PROXY='')
    p = run('self', 'config', '--mirror', 'auto', expected=0, offline=False)
    assert not any(x in p.stderr for x in ('Initialize', 'Bootstrap', 'Fetching')), p.stderr
    assert json.loads((target / 'registry' / '.xlings.json').read_text(encoding='utf-8'))['mirror'] == 'auto'
    assert sentinel.is_file()

    (original / 'home.toml').write_text('home = [\n', encoding='utf-8')
    p = run('self', 'env', '--format', 'json', expected=4)
    assert 'locator' in p.stderr and 'internal:' not in p.stderr
    run('self', 'init', '--force', '--home', str(target))
    assert sentinel.is_file()
    data = json.loads(run('self', 'env', '--format', 'json', expected=0).stdout)['data']
    assert Path(data['mcppHome']).resolve() == target

    run('self', 'init', '--force', '--home', str(original))
    assert not (original / 'home.toml').exists()
    assert sentinel.is_file()
    data = json.loads(run('self', 'env', '--format', 'json', expected=0).stdout)['data']
    assert Path(data['mcppHome']).resolve() == original

    # Portable installations own their locator and ignore the user's bad one.
    portable = root / 'portable'
    portable_bin = portable / 'bin' / binary.name
    portable_bin.parent.mkdir(parents=True)
    shutil.copy2(binary, portable_bin)
    (portable / 'home.toml').write_text('home = ' + json.dumps(target.as_posix(), ensure_ascii=False)
                                      + '\n', encoding='utf-8')
    portable_env = env.copy()
    portable_env.pop('MCPP_HOME')
    p = subprocess.run([str(portable_bin), 'self', 'env', '--format', 'json'],
                       env=portable_env, cwd=root, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, encoding='utf-8', timeout=15)
    assert p.returncode == 0, (p.returncode, p.stdout, p.stderr)
    data = json.loads(p.stdout)['data']
    assert Path(data['originalHome']).resolve() == portable
    assert Path(data['mcppHome']).resolve() == target
    assert user_locator.read_text(encoding='utf-8') == 'home = [\n'

    # Real concurrent writers and separate readers exercise the shared locator.
    raced = [root / ('concurrent home ' + str(i)) for i in range(4)]
    writers = []
    try:
        for destination in raced:
            writers.append(subprocess.Popen([str(binary), 'self', 'init', '--force', '--home',
                                              str(destination)], env=env, cwd=root,
                                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE))
        for _ in range(4):
            data = json.loads(run('self', 'env', '--format', 'json', expected=0).stdout)['data']
            assert Path(data['mcppHome']).resolve() in [original, *raced], data
        for writer in writers:
            out, err = writer.communicate(timeout=15)
            assert writer.returncode in (0, 1), (writer.returncode, out, err)
    finally:
        for writer in writers:
            if writer.poll() is None:
                writer.kill()
                writer.communicate(timeout=5)
    data = json.loads(run('self', 'env', '--format', 'json', expected=0).stdout)['data']
    assert Path(data['mcppHome']).resolve() in raced, data
    assert sentinel.is_file() and (original / 'config.toml').is_file()
    assert not list(original.glob('home.toml.*.tmp')), 'a normal writer left a staged locator'

    # Force a genuine publication/write failure and retain the previous pointer.
    locator = original / 'home.toml'
    before = locator.read_bytes()
    blocked = root / 'blocked publication'
    write_fault_proved = False
    if os.name == 'nt':
        locator.chmod(stat.S_IREAD)
        try:
            p = run('self', 'init', '--force', '--home', str(blocked), expected=1)
            assert locator.read_bytes() == before, (p.stdout, p.stderr)
            write_fault_proved = True
        finally:
            locator.chmod(stat.S_IREAD | stat.S_IWRITE)
    else:
        mode = stat.S_IMODE(original.stat().st_mode)
        original.chmod(0o500)
        try:
            control = original / 'write-fault-control'
            try:
                control.write_bytes(b'control')
            except PermissionError:
                p = run('self', 'init', '--force', '--home', str(blocked), expected=1)
                assert locator.read_bytes() == before, (p.stdout, p.stderr)
                write_fault_proved = True
            else:
                control.unlink()
                print('SKIP: this user bypasses directory write permissions; no POSIX write-fault proof')
        finally:
            original.chmod(mode)

    assert not list(original.glob('home.toml.*.tmp')), 'a failed publication left a staged locator'

    # Bare force still resets the effective target, retaining origin and locator.
    run('self', 'init', '--force', '--home', str(target))
    retained = original / 'registry' / 'retained-old-data'
    retained.parent.mkdir(exist_ok=True)
    retained.write_text('keep', encoding='utf-8')
    markers = []
    for name in ('registry', 'cache', 'build-cache', 'bmi'):
        folder = target / name
        folder.mkdir(exist_ok=True)
        marker = folder / 'bare-force-marker'
        marker.write_text('derived', encoding='utf-8')
        markers.append(marker)
    p = run('self', 'init', '--force')
    assert p.returncode in (0, 1), (p.stdout, p.stderr)
    assert '--force rebuilds' in p.stderr and target.name in p.stderr, p.stderr
    assert all(not marker.exists() for marker in markers)
    assert retained.read_text(encoding='utf-8') == 'keep' and locator.is_file()
    data = json.loads(run('self', 'env', '--format', 'json', expected=0).stdout)['data']
    assert Path(data['mcppHome']).resolve() == target

print('PASS: Unicode/relative persistence, separate env/portable locators, concurrent publication, and force data boundaries; write-fault retention ' + ('verified' if write_fault_proved else 'unavailable on this account'))
