#!/usr/bin/env python3
"""Candidate admission preserves configuration and refuses stale/fork binaries."""
import copy
import importlib.util
from pathlib import Path
import tempfile
import os
import subprocess
import textwrap
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'.github/tools/{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


seed = load('seed_native_xim_index')
download = load('download_native_admission_mcpp')


class CandidateAdmission(unittest.TestCase):
    def test_fresh_and_existing_home_use_same_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout = root / 'candidate'
            (checkout / 'pkgs/l').mkdir(parents=True)
            (checkout / 'pkgs/l/llvm.lua').write_text('return {}')
            for name in ('outer', 'cold'):
                home = root / name
                home.mkdir()
                if name == 'outer':
                    (home/'config.toml').write_text('[cache]\nvalue = 7\n[index.repos."xim"]\nurl = "old"\nnote = "preserved"\n[index.repos.other]\nurl = "elsewhere"\n')
                seed.seed(home, checkout)
                before = (home/'config.toml').read_text()
                seed.seed(home, checkout)
                self.assertEqual(before, (home/'config.toml').read_text())
                data = tomllib.loads(before)
                self.assertEqual(str(checkout), data['index']['repos']['xim']['url'])
                if name == 'outer':
                    self.assertEqual(7, data['cache']['value'])
                    self.assertEqual('preserved', data['index']['repos']['xim']['note'])
                    self.assertEqual('elsewhere', data['index']['repos']['other']['url'])

    def test_missing_recipe_refuses_to_seed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ValueError): seed.seed(root/'home', root)
            self.assertFalse((root/'home/config.toml').exists())

    def test_effective_registry_verification_refuses_main(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout = root / 'candidate'
            checkout.mkdir()
            home = root / 'home'
            (home/'registry').mkdir(parents=True)
            registry = home/'registry/.xlings.json'
            registry.write_text(json.dumps({'index_repos': [{'name': 'xim', 'url': 'main'}]}))
            with self.assertRaises(ValueError): seed.verify(home, checkout)
            registry.write_text(json.dumps({'index_repos': [{'name': 'xim', 'url': str(checkout)}]}))
            seed.verify(home, checkout)

    def test_consumer_adapter_adds_flags_only_to_build_test_run(self):
        workflow = (ROOT/'.github/workflows/ci-aarch64-fresh-install.yml').read_text()
        block = workflow.split("cat > \"$adapter\" <<'SH'\n", 1)[1].split('\n          SH\n', 1)[0]
        adapter_source = textwrap.dedent(block)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            adapter = root/'adapter'
            adapter.write_text(adapter_source)
            adapter.chmod(0o755)
            binary = root/'mcpp'
            binary.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@" > "$RECORDED_ARGS"\n')
            binary.chmod(0o755)
            recorded = root/'argv'
            env = dict(os.environ, MCPP_NATIVE_GNU=str(binary), MCPP_NATIVE_REPORT_DIR=str(root), RECORDED_ARGS=str(recorded))
            for command in ('build', 'test', 'run', '--version', 'self', 'index'):
                subprocess.run([str(adapter), command], env=env, check=True)
                args = recorded.read_text().splitlines()
                expected = [command] + (['--toolchain', 'llvm@23.1.3', '--target', 'aarch64-linux-gnu']
                                        if command in ('build', 'test', 'run') else [])
                self.assertEqual(expected, args)

    def valid_source(self):
        repository, commit = 'mcpp-community/mcpp', 'a'*40
        run = {'repository': {'full_name': repository}, 'head_repository': {'full_name': repository},
               'head_sha': commit, 'path': '.github/workflows/ci.yml', 'conclusion': 'failure'}
        jobs = [{'name': 'build-linux-arm / build mcpp (linux-aarch64)', 'head_sha': commit,
                 'status': 'completed', 'conclusion': 'success'}]
        artifacts = [{'name': 'mcpp-built-linux-aarch64', 'expired': False}]
        return run, jobs, artifacts, repository, commit

    def test_successful_build_accepted_despite_another_failed_job(self):
        download.validate(*self.valid_source())

    def test_stale_fork_or_unsuccessful_builds_rejected(self):
        for problem in ('repo', 'fork', 'head', 'workflow', 'job_sha', 'job_failed', 'job_pending', 'expired', 'duplicate'):
            with self.subTest(problem=problem):
                run, jobs, artifacts, repository, commit = copy.deepcopy(self.valid_source())
                if problem == 'repo': run['repository']['full_name'] = 'other/repo'
                elif problem == 'fork': run['head_repository']['full_name'] = 'fork/repo'
                elif problem == 'head': run['head_sha'] = 'b'*40
                elif problem == 'workflow': run['path'] = '.github/workflows/other.yml'
                elif problem == 'job_sha': jobs[0]['head_sha'] = 'b'*40
                elif problem == 'job_failed': jobs[0]['conclusion'] = 'failure'
                elif problem == 'job_pending': jobs[0]['status'] = 'in_progress'
                elif problem == 'expired': artifacts[0]['expired'] = True
                elif problem == 'duplicate': artifacts.append(dict(artifacts[0]))
                with self.assertRaises(ValueError): download.validate(run, jobs, artifacts, repository, commit)


if __name__ == '__main__':
    unittest.main()
