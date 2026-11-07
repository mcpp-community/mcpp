#!/usr/bin/env python3
"""A completed compiler cannot fail because status and liveness were sampled apart."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mcpp', type=Path)
    parser.add_argument('project', type=Path)
    args = parser.parse_args()
    mcpp = args.mcpp.resolve()
    if not Path('/proc/self/stat').exists() or not Path('/usr/bin/cc').exists():
        print('SKIP: completion observation requires Linux libc interposition')
        return
    elf = subprocess.run(['readelf', '-l', str(mcpp)], capture_output=True, text=True, check=True)
    if 'ld-linux' not in elf.stdout:
        print('SKIP: completion observation requires a dynamic glibc mcpp')
        return
    root = args.project.resolve()
    (root / 'src').mkdir(parents=True)
    (root / 'mcpp.toml').write_text('''[package]
name = "completion-observation"
version = "0.1.0"
[build]
bmi_schedule = "on"
default-profile = "release"
[toolchain]
default = "gcc@16.1.0"
''')
    (root / 'src/value.cppm').write_text('export module race.value;\nexport int value(){return 42;}\n')
    (root / 'src/main.cpp').write_text('import race.value;\nint main(){return value()==42?0:1;}\n')
    library = root / 'completion-race.so'
    subprocess.run(['/usr/bin/cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                    str(Path(__file__).with_name('compiler_completion_race.c')),
                    '-o', str(library), '-ldl'], check=True)
    env = dict(os.environ, LD_PRELOAD=str(library))
    result = subprocess.run([str(mcpp), 'build'], cwd=root, env=env,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert list((root / 'target').rglob('*.completion-race')), 'the completion race was not exercised'
    programs = list((root / 'target').glob('*/*/bin/completion-observation'))
    assert len(programs) == 1, programs
    subprocess.run([str(programs[0])], check=True, timeout=10)
    result = subprocess.run([str(mcpp), 'build'], cwd=root,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    print('PASS: a final status published before controller exit survives an earlier absent observation')


if __name__ == '__main__':
    main()
