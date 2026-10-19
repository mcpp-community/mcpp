import json
import os
from pathlib import Path
import subprocess
import tempfile

binary = Path(os.environ['MCPP']).resolve()
with tempfile.TemporaryDirectory(prefix='mcpp-home-e2e-') as tmp:
    root = Path(tmp).resolve()
    original, target, third = (root / name for name in ('original', 'selected', 'third'))
    original.mkdir()
    (original / 'config.toml').write_text('# Existing home\n', encoding='utf-8')
    env = os.environ | {'MCPP_HOME': str(original), 'MCPP_OFFLINE': '1'}

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

    p = run('self', 'init', '--home', str(target), expected=1)
    assert '--force' in p.stderr and not target.exists(), (p.stdout, p.stderr)
    p = run('self', 'init', '--force', '--home', str(target))
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
    assert json.loads((target / 'registry' / '.xlings.json').read_text())['mirror'] == 'auto'
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

print('PASS: a home selection persists once; explicit force-home preserves data; mirror recovery stays local')
