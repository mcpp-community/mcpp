#!/usr/bin/env python3
"""Reuse the current head's successful native build, even if another job failed."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def validate(run, jobs, artifacts, repository, commit):
    if run.get('repository', {}).get('full_name') != repository or run.get('head_repository', {}).get('full_name') != repository:
        raise ValueError('build run must have the same repository and head repository')
    if run.get('head_sha') != commit or not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('build run does not belong to the dispatched mcpp head')
    if run.get('path') != '.github/workflows/ci.yml':
        raise ValueError('build run must use the staged mcpp CI workflow')
    native = [job for job in jobs if job.get('name') == 'build-linux-arm / build mcpp (linux-aarch64)']
    if len(native) != 1 or native[0].get('head_sha') != commit or native[0].get('status') != 'completed' or native[0].get('conclusion') != 'success':
        raise ValueError('this head has no successful completed native mcpp build job')
    found = [a for a in artifacts if a.get('name') == 'mcpp-built-linux-aarch64' and not a.get('expired')]
    if len(found) != 1:
        raise ValueError('this native build has no unique unexpired mcpp artifact')


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', '--paginate', '--slurp', path], text=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_id')
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    if not re.fullmatch('[0-9]+', args.run_id):
        raise ValueError('run id must contain only digits')
    repository = os.environ['GITHUB_REPOSITORY']
    commit = os.environ['GITHUB_SHA']
    base = f'repos/{repository}/actions/runs/{args.run_id}'
    run = api(base)[0]
    jobs = [job for page in api(base + '/jobs') for job in page['jobs']]
    artifacts = [artifact for page in api(base + '/artifacts') for artifact in page['artifacts']]
    validate(run, jobs, artifacts, repository, commit)
    args.directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(['gh', 'run', 'download', args.run_id, '--repo', repository,
                    '--name', 'mcpp-built-linux-aarch64', '--dir', str(args.directory)], check=True)
    binary = args.directory / 'mcpp'
    if not binary.is_file():
        raise ValueError('native build artifact does not contain mcpp')
    binary.chmod(0o755)
    print(f'Admission binary: {repository} commit {commit}, native build run {args.run_id}, {binary}')


if __name__ == '__main__':
    main()
