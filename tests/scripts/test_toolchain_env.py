"""The E2E helper selects payloads from the effective mcpp registry."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'e2e' / '_toolchain_env.sh'


class ToolchainRegistry(unittest.TestCase):
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
