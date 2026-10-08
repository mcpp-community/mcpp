"""Artifact consumers install the manifest toolchain used to build each host."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / '.github/actions/use-built-mcpp/use.sh'


class ArtifactToolchain(unittest.TestCase):
    def run_host(self, host):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'mcpp.toml').write_text('[toolchain]\ndefault = "gcc@16.1.0"\n\n[target.aarch64-linux-gnu]\ntoolchain = "llvm@23.1.3"\n')
            artifact = root / 'mcpp-built'
            artifact.mkdir()
            binary = artifact / 'mcpp'
            binary.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$*" >> "$CALLS"\n')
            binary.chmod(0o755)
            calls = root / 'calls'
            env = dict(os.environ, RUNNER_TEMP=directory, MCPP=str(binary),
                       CALLS=str(calls), GITHUB_ENV=str(root / 'env'))
            env.pop('XLINGS_BIN', None)
            subprocess.run(['bash', str(SCRIPT), host, 'GLOBAL'], cwd=root, env=env,
                           capture_output=True, text=True, check=True)
            return calls.read_text()

    def test_arm64_consumer_installs_native_override(self):
        calls = self.run_host('linux-aarch64')
        self.assertIn('toolchain install llvm 23.1.3', calls)
        self.assertNotIn('toolchain install gcc', calls)

    def test_x86_consumer_retains_platform_default(self):
        calls = self.run_host('linux-x86_64')
        self.assertIn('toolchain install gcc 16.1.0', calls)
