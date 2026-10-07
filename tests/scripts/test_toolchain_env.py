"""The E2E helper selects payloads from the effective mcpp registry."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'e2e' / '_toolchain_env.sh'


class ToolchainRegistry(unittest.TestCase):
    def test_real_gcc_compiles_after_helper_without_losing_user_prefix(self):
        compiler = os.environ.get('MCPP_E2E_GCC_DRIVER') or shutil.which('g++')
        if not compiler:
            self.skipTest('a real GCC C++ driver is unavailable')
        version = subprocess.run([compiler, '--version'], capture_output=True, text=True, check=True)
        if 'clang' in version.stdout.lower():
            self.skipTest('g++ is a Clang alias')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'probe.cpp'
            source.write_text('int answer() { return 42; }\n')
            env = dict(os.environ, MCPP_HOME=str(root / 'cold-home'))
            env.pop('GCC_ROOT', None)
            args = [compiler, '-c', str(source), '-o', str(root / 'probe.o')]
            clean = subprocess.run(args, env=env, capture_output=True, text=True)
            self.assertEqual(clean.returncode, 0, clean.stderr)
            # A nonexistent explicit compiler prefix reproduces the exact
            # missing-cc1plus failure from the isolated Windows fixture.
            bad_env = dict(env, GCC_ROOT=str(root / 'missing-gcc-root'))
            bad = subprocess.run(args, env=bad_env, capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn('cc1plus', bad.stderr)
            for context, expected in ((env, 0), (bad_env, bad.returncode)):
                after = subprocess.run(
                    ['bash', '-c', 'source "$1"; shift; "$@"', 'bash', str(HELPER), *args],
                    env=context, capture_output=True, text=True)
                self.assertEqual(after.returncode, expected, after.stderr)

    def test_fixture_paths_do_not_change_gcc_child_process_prefix(self):
        # GCC interprets GCC_ROOT itself; exporting a fixture registry root
        # makes a cold MinGW driver search another installation for cc1plus.
        for existing in (None, '/explicit/compiler/root'):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as directory:
                env = dict(os.environ, MCPP_HOME=directory)
                env.pop('GCC_ROOT', None)
                if existing is not None:
                    env['GCC_ROOT'] = existing
                result = subprocess.run(
                    ['bash', '-c', 'source "$1"; bash -c \'printf "%s" "${GCC_ROOT-unset}"\'',
                     'bash', str(HELPER)], env=env, capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout, existing if existing is not None else 'unset')

    def test_explicit_home_outranks_home_and_userprofile(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for base, version in ((root / 'home' / '.mcpp', '99.0.0'),
                                  (root / 'profile' / '.mcpp', '98.0.0'),
                                  (root / 'custom', '23.1.3')):
                (base / 'registry/data/xpkgs/xim-x-llvm' / version).mkdir(parents=True)
            env = dict(os.environ, HOME=str(root / 'home'), USERPROFILE=str(root / 'profile'),
                       MCPP_HOME=str(root / 'custom'))
            env.pop('MCPP_E2E_LLVM_VERSION', None)
            result = subprocess.run(['bash', '-c', 'source "$1"; printf "%s\\n%s\\n" "$LLVM_VERSION" "$LLVM_ROOT"',
                                     'bash', str(HELPER)], env=env, capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.splitlines(),
                             ['23.1.3', str(root / 'custom/registry/data/xpkgs/xim-x-llvm/23.1.3')])

    def test_explicit_version_outranks_installed_payloads(self):
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, MCPP_HOME=directory, MCPP_E2E_LLVM_VERSION='24.0.1')
            result = subprocess.run(['bash', '-c', 'source "$1"; printf "%s" "$LLVM_VERSION"',
                                     'bash', str(HELPER)], env=env, capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout, '24.0.1')
