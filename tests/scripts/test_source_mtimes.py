#!/usr/bin/env python3
"""The sources' times for a build restored from main's run
(.github/tools/source_mtimes.py, design 2026-11-14 §6.2 W1); offline."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / ".github/tools/source_mtimes.py"
OLD = 946684800


def git(cwd: Path, *args: str) -> None:
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args],
                   cwd=cwd, check=True, capture_output=True)


def tool(cwd: Path, *args: str) -> str:
    return subprocess.run([sys.executable, str(SCRIPT), *args], cwd=cwd, check=True,
                          capture_output=True, text=True).stdout


class SourceMtimes(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        git(self.root, "init", "-q")
        for name in ("same.cpp", "changed.cpp", "edited.cpp"):
            (self.root / name).write_text(name)
        git(self.root, "add", "-A")
        git(self.root, "commit", "-qm", "a")
        tool(self.root, "record", "target")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_an_unchanged_file_is_older_than_the_build(self) -> None:
        # The next commit changes one file; the working tree edits another and adds one.
        (self.root / "changed.cpp").write_text("changed again")
        git(self.root, "commit", "-qam", "b")
        (self.root / "edited.cpp").write_text("edited")
        (self.root / "new.cpp").write_text("new")
        git(self.root, "add", "new.cpp")
        out = tool(self.root, "restore", "target")
        self.assertIn("1 file(s) as recorded, 3 changed or new", out)
        self.assertEqual(os.stat(self.root / "same.cpp").st_mtime, OLD)
        for name in ("changed.cpp", "edited.cpp", "new.cpp"):
            self.assertGreater(os.stat(self.root / name).st_mtime, OLD, name)

    def test_without_a_record_nothing_changes(self) -> None:
        before = os.stat(self.root / "same.cpp").st_mtime
        out = tool(self.root, "restore", "elsewhere")
        self.assertIn("rebuilt whole", out)
        self.assertEqual(os.stat(self.root / "same.cpp").st_mtime, before)


if __name__ == "__main__":
    unittest.main()
