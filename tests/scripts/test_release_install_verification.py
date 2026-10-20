#!/usr/bin/env python3
"""Run the real Homebrew verification step against controlled command results.

An old formula and its matching old binary must not validate a newer release.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ci-fresh-install.yml"
spec = importlib.util.spec_from_file_location(
    "release_ci_assertions", ROOT / ".github/tools/check_workflow_assertions.py"
)
parser = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = parser
spec.loader.exec_module(parser)


def run_guard(formula: str, binary: str) -> subprocess.CompletedProcess[str]:
    workflow = parser.parse(WORKFLOW)
    job = next(j for j in workflow.jobs if j.key == "macos-brew-fresh")
    step = next(s for s in job.steps if s.name.startswith("Installed binary is brew's"))
    with tempfile.TemporaryDirectory() as directory:
        prefix = Path(directory) / "brew"
        bins = prefix / "bin"
        bins.mkdir(parents=True)
        (bins / "brew").write_text(r'''#!/bin/sh
case "$1" in
  --prefix) printf '%s\n' "$CI_STUB_PREFIX" ;;
  info) printf '{"formulae":[{"versions":{"stable":"%s"}}]}\n' "$CI_STUB_FORMULA" ;;
  *) exit 99 ;;
esac
''')
        (bins / "mcpp").write_text(r'''#!/bin/sh
[ "$1" = --version ] || exit 99
printf 'mcpp %s\n' "$CI_STUB_BINARY"
''')
        for name in ("brew", "mcpp"):
            (bins / name).chmod(0o755)
        env = dict(os.environ, PATH=str(bins) + os.pathsep + os.environ["PATH"],
                   CI_STUB_PREFIX=str(prefix), CI_STUB_FORMULA=formula,
                   CI_STUB_BINARY=binary, MCPP_PIN="2026.10.21.1")
        return subprocess.run(["bash", "-c", step.run], env=env,
                              capture_output=True, text=True, check=False)


class ReleasedHomebrewVersion(unittest.TestCase):
    def test_matching_old_formula_and_binary_do_not_validate_the_new_release(self):
        result = run_guard("2026.10.19.1", "2026.10.19.1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_the_requested_release_passes(self):
        result = run_guard("2026.10.21.1", "2026.10.21.1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_wrong_binary_is_rejected_even_with_the_new_formula(self):
        result = run_guard("2026.10.21.1", "2026.10.19.1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
