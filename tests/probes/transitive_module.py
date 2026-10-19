"""Observe #816 with a local dependency, including direct-import controls."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

out = Path('module-probe-results').resolve()
out.mkdir(exist_ok=True)
root = Path(tempfile.mkdtemp(prefix='mcpp-module-probe-'))
env = os.environ.copy()
env['MCPP_OFFLINE'] = '1'
env['MCPP_HOME'] = str(Path.home() / '.mcpp')
rows = []
try:
    dep = root / 'dep'
    (dep / 'src').mkdir(parents=True)
    (dep / 'mcpp.toml').write_text('''[package]
name = "dep"
version = "0.1.0"
[language]
standard = "c++23"
import_std = false
[targets.dep]
kind = "lib"
''')
    (dep / 'src' / 'dep.cppm').write_text('export module depmod;\nexport int answer() { return 42; }\n')
    for mode in ('direct', 'transitive', 'header-transitive'):
        app = root / mode
        (app / 'src').mkdir(parents=True)
        (app / 'mcpp.toml').write_text('''[package]
name = "probe"
version = "0.1.0"
[language]
standard = "c++23"
import_std = false
[dependencies]
dep = { path = "../dep" }
''')
        if mode == 'direct':
            source = 'import depmod;\nint main() { return answer() == 42 ? 0 : 1; }\n'
        else:
            (app / 'src' / 'mylib.cppm').write_text('export module mylib;\nimport depmod;\nexport int value() { return answer(); }\n')
            source = 'import mylib;\nint main() { return value() == 42 ? 0 : 1; }\n'
            if mode == 'header-transitive':
                (app / 'src' / 'helper.h').write_text('#pragma once\nimport mylib;\n')
                source = '#include "helper.h"\nint main() { return value() == 42 ? 0 : 1; }\n'
        (app / 'src' / 'main.cpp').write_text(source)
        start = time.monotonic()
        # The Actions step supplies an outer timeout as well.
        result = subprocess.run([env['MCPP'], 'build'], cwd=app, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        (out / (mode + '.log')).write_bytes(result.stdout)
        row = dict(mode=mode, rc=result.returncode, seconds=round(time.monotonic()-start, 3))
        rows.append(row)
        print(json.dumps(row), flush=True)
        db = app / 'compile_commands.json'
        if db.exists():
            shutil.copyfile(db, out / (mode + '-compile_commands.json'))
        for n, ninja in enumerate(app.rglob('build.ninja')):
            shutil.copyfile(ninja, out / (mode + f'-{n}-build.ninja'))
    assert rows[0]['rc'] == 0, 'direct-import control failed; cannot interpret transitive results'
finally:
    (out / 'results.json').write_text(json.dumps(rows, indent=2))
    shutil.rmtree(root, ignore_errors=True)
