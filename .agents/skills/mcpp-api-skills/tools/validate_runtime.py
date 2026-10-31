#!/usr/bin/env python3
"""Exercise real CLI behavior in temporary projects; never publish or use devices."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


def validate(binary: Path, build: bool = False, build_home: Path | None = None) -> dict:
    results = []
    with tempfile.TemporaryDirectory(prefix='mcpp-api-skills-') as directory:
        root = Path(directory)
        env = os.environ.copy()
        # Caller constraints must not change between probes. Toolchain and cache
        # locations remain isolated; no global install or repository edit occurs.
        env['MCPP_HOME'] = str(build_home if build_home else root / 'home')
        env['MCPP_CLIENT'] = 'mcpp-api-skills-validation'

        def run(name, args, predicate, cwd=root):
            try:
                proc = subprocess.run([str(binary), *args], cwd=cwd, env=env,
                                      capture_output=True, text=True, encoding='utf-8',
                                      errors='replace', timeout=120 if build else 30)
                try:
                    passed = bool(predicate(proc))
                except (ValueError, KeyError, TypeError):
                    passed = False
                row = dict(name=name, args=args, returncode=proc.returncode, passed=passed,
                           stdout=proc.stdout[-12000:], stderr=proc.stderr[-12000:])
            except (OSError, subprocess.TimeoutExpired) as exc:
                row = dict(name=name, args=args, passed=False, error=str(exc))
            results.append(row)
            return row

        run('version', ['--version'], lambda p: p.returncode == 0 and '2026.' in p.stdout)
        run('protocol-detection', ['--protocol-version'], lambda p:
            p.returncode == 0 and json.loads(p.stdout)['kind'] == 'mcpp.protocol'
            and json.loads(p.stdout)['schemaVersion'] == 1
            and 'test-launch' in json.loads(p.stdout)['features'])
        run('env-envelope', ['self', 'env', '--format', 'json'], lambda p:
            p.returncode == 0 and json.loads(p.stdout)['kind'] == 'mcpp.env')
        run('cache-envelope', ['cache', 'list', '--format', 'json'], lambda p:
            p.returncode == 0 and json.loads(p.stdout)['kind'] == 'mcpp.cache')
        run('usage-error', ['build', '--not-a-real-option'], lambda p:
            p.returncode == 2 and bool(p.stderr.strip()))
        run('unknown-command', ['not-a-real-command'], lambda p:
            p.returncode != 0 and bool(p.stderr.strip()))
        run('deny-unknown-effect', ['--deny', 'not-an-effect', 'build'], lambda p:
            p.returncode == 2 and bool(p.stderr.strip()))
        run('new-project', ['new', 'hello'], lambda p:
            p.returncode == 0 and (root / 'hello/mcpp.toml').is_file()
            and (root / 'hello/src/main.cpp').is_file()
            and bool(list((root / 'hello/tests').glob('*.cpp'))))
        project = root / 'hello'
        if project.is_dir():
            run('test-discovery', ['test', '--list'], lambda p:
                p.returncode == 0 and 'smoke' in p.stdout.lower(), project)
            run('no-overwrite', ['new', 'hello'], lambda p:
                p.returncode != 0 and bool(p.stderr.strip()))
            if build:
                # Offline deliberately avoids silently downloading a toolchain.
                # A missing prerequisite is reported as a failed execution, never
                # accepted as compile coverage.
                row = run('compile-project-offline', ['--offline', 'build'],
                          lambda p: p.returncode == 0, project)
                if row['passed']:
                    run('run-project-offline', ['--offline', 'run'], lambda p: p.returncode == 0, project)
                    run('test-project-offline', ['--offline', 'test'], lambda p: p.returncode == 0, project)
                    run('build-ndjson', ['--offline', 'build', '--message-format', 'json'], lambda p:
                        p.returncode == 0 and json.loads(p.stdout.splitlines()[-1])['kind'] == 'mcpp.build', project)
                    run('launch-envelope', ['--offline', 'run', '--print-launch'], lambda p:
                        p.returncode == 0 and json.loads(p.stdout)['kind'] == 'mcpp.launch', project)
    return dict(binary=str(binary), provenance='caller-selected binary; not assumed fresh',
                build_requested=build, checks=results,
                passed=all(row['passed'] for row in results))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mcpp', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--build-home', type=Path,
                        help='Prepared, isolated mcpp home with provider/toolchain already initialized; required for --build')
    args = parser.parse_args()
    if args.build and (args.build_home is None or not args.build_home.is_dir()):
        parser.error('--build needs --build-home pointing to a prepared isolated home; compilation stays offline')
    report = validate(args.mcpp.resolve(), args.build,
                      args.build_home.resolve() if args.build_home else None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    for check in report['checks']:
        print(('PASS' if check['passed'] else 'FAIL') + ': ' + check['name'])
    return int(not report['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
