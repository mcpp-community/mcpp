#!/usr/bin/env python3
"""Fixture tests for .github/tools/check_default_toolchain_docs.py (WS8).

The check must pass on the repository's tables for every host row with the
spec each row states, and fail on a table that states another version.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".github" / "tools" / "check_default_toolchain_docs.py"
DOCS = ["docs/01-getting-started.md", "docs/zh/01-getting-started.md",
        "docs/20-toolchains.md", "docs/zh/20-toolchains.md"]
ROWS = [("Linux", "x86_64", "gcc@16.1.0"), ("Linux", "aarch64", "llvm@23.1.3"), ("Linux", "riscv64", "gcc@15.1.0-musl"),
        ("Darwin", "arm64", "llvm@23.1.3"), ("Windows", "AMD64", "llvm@23.1.3"),
        ("Windows", "AMD64", "gcc@16.1.0")]


def run(root: Path, os_name: str, arch: str, spec: str) -> int:
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root),
                           "--os", os_name, "--arch", arch, "--spec", spec],
                          capture_output=True, text=True, check=False).returncode


class DefaultToolchainDocs(unittest.TestCase):
    def test_every_row_of_the_repository_states_its_default(self) -> None:
        for os_name, arch, spec in ROWS:
            with self.subTest(os=os_name, arch=arch):
                self.assertEqual(run(REPO_ROOT, os_name, arch, spec), 0)

    def test_a_table_that_states_another_version_fails(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for rel in DOCS:
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(REPO_ROOT / rel, root / rel)
            self.assertEqual(run(root, "Darwin", "arm64", "llvm@23.1.3"), 0)
            zh = root / "docs/zh/01-getting-started.md"
            zh.write_text(zh.read_text(encoding="utf-8").replace(
                "| macOS | `llvm@23.1.3` |", "| macOS | `llvm@22.1.8` |"), encoding="utf-8")
            self.assertEqual(run(root, "Darwin", "arm64", "llvm@23.1.3"), 1)

    def test_a_different_answer_fails_against_the_tables(self) -> None:
        self.assertEqual(run(REPO_ROOT, "Darwin", "arm64", "llvm@22.1.8"), 1)


if __name__ == "__main__":
    unittest.main()
