"""Observe released mcpp behavior; no proposed home/network implementation.

Each child has a deadline and is killed as a tree. Proxy refusal is an injected
failure, not a simulation of every DNS/TLS/packet-loss failure.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('--release', required=True)
p.add_argument('--out', required=True)
a = p.parse_args()
release = Path(a.release).resolve()
out = Path(a.out).resolve()
out.mkdir(parents=True, exist_ok=True)
scratch = Path(tempfile.mkdtemp(prefix='mcpp-first-run-'))
exe = 'mcpp.exe' if os.name == 'nt' else 'mcpp'
binary = release / 'bin' / exe
assert binary.is_file(), binary
provider = next(release.rglob('xlings.exe' if os.name == 'nt' else 'xlings'))
env = os.environ.copy()
for k in ('MCPP_HOME', 'MCPP_OFFLINE', 'MCPP_SYSTEM_CONFIG'):
    env.pop(k, None)
env.update(HOME=str(scratch / 'user'), USERPROFILE=str(scratch / 'user'),
           MCPP_VENDORED_XLINGS=str(provider), XLINGS_NON_INTERACTIVE='1')
(scratch / 'user').mkdir()
rows = []

def run(name, args, changes=None, executable=None, limit=20):
    childenv = env | (changes or {})
    cmd = [str(executable or binary), *args]
    start = time.monotonic()
    log = out / (name + '.log')
    timed_out = False
    with log.open('wb') as f:
        proc = subprocess.Popen(cmd, env=childenv, cwd=scratch, stdin=subprocess.DEVNULL,
                                stdout=f, stderr=subprocess.STDOUT,
                                start_new_session=os.name != 'nt')
        try:
            rc = proc.wait(timeout=limit)
        except subprocess.TimeoutExpired:
            timed_out = True
            if os.name == 'nt':
                subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            else:
                os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=10)
            rc = proc.returncode
    text = log.read_text(encoding='utf-8', errors='replace')
    row = dict(name=name, command=args, rc=rc, timed_out=timed_out,
               seconds=round(time.monotonic() - start, 3))
    rows.append(row)
    print(json.dumps(row), flush=True)
    return text, row

def home_of(text):
    data = json.loads(text)
    return Path(data.get('data', data)['mcppHome'])

try:
    run('version', ['--version'])
    explicit = scratch / 'explicit'
    text, row = run('explicit-json', ['--offline', 'self', 'env', '--format', 'json'],
                    {'MCPP_HOME': str(explicit)})
    assert row['rc'] == 0 and home_of(text) == explicit
    assert not explicit.exists(), 'JSON env unexpectedly initialized the home'

    text, row = run('portable-json', ['--offline', 'self', 'env', '--format', 'json'])
    assert row['rc'] == 0 and home_of(text) == release

    # The dev layout keeps the same executable-relative runtime layout while
    # excluding self-contained mode through the existing target-ancestor rule.
    devroot = scratch / 'target' / 'probe'
    shutil.copytree(release, devroot)
    devbin = devroot / 'bin' / exe
    text, row = run('fallback-json', ['--offline', 'self', 'env', '--format', 'json'],
                    executable=devbin)
    assert row['rc'] == 0 and home_of(text) == scratch / 'user' / '.mcpp'

    offline_home = scratch / 'offline'
    text, row = run('offline-doctor', ['--offline', 'self', 'doctor'],
                    {'MCPP_HOME': str(offline_home)})
    assert not row['timed_out']
    assert not (offline_home / 'registry' / 'data' / 'xim-pkgindex').exists()
    assert not (offline_home / 'registry' / 'subos' / 'default' / '.xlings.json').exists()
    assert 'Checking build policy' in text, 'doctor did not reach later diagnostics'

    force_home = scratch / 'force'
    removed = ('registry', 'cache', 'build-cache', 'bmi')
    for name in (*removed, 'keep'):
        d = force_home / name
        d.mkdir(parents=True)
        (d / 'probe-sentinel').write_text('retain?', encoding='utf-8')
    text, row = run('force-offline', ['--offline', 'self', 'init', '--force'],
                    {'MCPP_HOME': str(force_home)})
    row['removed_sentinels'] = [n for n in removed if not (force_home / n / 'probe-sentinel').exists()]
    row['unrelated_retained'] = (force_home / 'keep' / 'probe-sentinel').exists()
    assert len(row['removed_sentinels']) == 4 and row['unrelated_retained']

    # Inject a closed proxy port; output is evidence, not a green assertion that
    # ordinary doctor is bounded (the baseline may exceed this observation).
    proxy = dict(HTTPS_PROXY='http://127.0.0.1:9', HTTP_PROXY='http://127.0.0.1:9',
                 ALL_PROXY='http://127.0.0.1:9', https_proxy='http://127.0.0.1:9',
                 http_proxy='http://127.0.0.1:9', all_proxy='http://127.0.0.1:9',
                 NO_PROXY='', no_proxy='')
    for name, args in (
        ('blocked-doctor', ['self', 'doctor']),
        ('blocked-config', ['self', 'config', '--mirror', 'GLOBAL']),
    ):
        h = scratch / name
        text, row = run(name, args, proxy | {'MCPP_HOME': str(h)}, limit=12)
        row['started_bootstrap'] = any(s in text for s in ('Initialize', 'Bootstrap', 'Fetching'))
        cfg = h / 'registry' / '.xlings.json'
        row['seeded_mirror'] = json.loads(cfg.read_text()).get('mirror') if cfg.exists() else None

    inventory = dict(os=platform.platform(), python=platform.python_version())
    if os.name == 'nt':
        drive_type = ctypes.windll.kernel32.GetDriveTypeW('D:\\')
        inventory['d_drive_type'] = drive_type
        inventory['d_writable'] = False
        if drive_type == 3:
            try:
                with tempfile.TemporaryDirectory(prefix='mcpp-probe-', dir='D:\\'):
                    inventory['d_writable'] = True
            except OSError as e:
                inventory['d_write_error'] = type(e).__name__
    (out / 'inventory.json').write_text(json.dumps(inventory, indent=2), encoding='utf-8')
finally:
    (out / 'results.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    shutil.rmtree(scratch, ignore_errors=True)
