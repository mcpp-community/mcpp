#!/usr/bin/env python3
"""Fixture tests for .github/tools/check_e2e_coverage.py (rule R5).

A test that ran, a test a workflow names, and a test the exceptions excuse are
covered; a test that is none of these fails the check, and so does an exception
for a test that ran or that does not exist.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".github" / "tools" / "check_e2e_coverage.py"


class E2ECoverage(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)
        e2e = self.root / "tests" / "e2e"
        e2e.mkdir(parents=True)
        for name in ("10_runs.sh", "20_in_a_job.sh", "30_excused.sh", "40_by_pattern.sh"):
            (e2e / name).write_text("#!/usr/bin/env bash\n# requires: unix-shell\n")
        wf = self.root / ".github" / "workflows"
        wf.mkdir(parents=True)
        (wf / "ci.yml").write_text("run: bash tests/e2e/20_in_a_job.sh\nE2E_ONLY: '40_*.sh'\n")
        (e2e / "coverage-exceptions.tsv").write_text("# test\treason\n30_excused.sh\tneeds a device\n")
        self.reports = self.root / "reports"
        self.reports.mkdir()
        (self.reports / "e2e-report-linux-1.tsv").write_text(
            "pass\t10_runs.sh\t1200\t\nskip\t20_in_a_job.sh\t0\tmissing capability: qemu-riscv\n")

    def tearDown(self) -> None:
        self._dir.cleanup()

    def run_check(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root),
                               "--reports", str(self.reports), *extra],
                              capture_output=True, text=True, check=False)

    def test_ran_named_and_excused_tests_are_covered(self) -> None:
        r = self.run_check()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("1 ran on a shard, 2 run by a dedicated job, 1 excused, 0 uncovered", r.stdout)

    def test_a_name_in_a_comment_or_inside_a_longer_name_does_not_count(self) -> None:
        e2e = self.root / "tests" / "e2e"
        (e2e / "60_commented.sh").write_text("# x\n# requires: llvm\n")
        (e2e / "70_short.sh").write_text("# x\n# requires: llvm\n")
        with (self.root / ".github" / "workflows" / "ci.yml").open("a") as f:
            f.write("      # 60_commented.sh is mentioned in a comment only\n"
                    "run: bash tests/e2e/170_short_but_longer.sh\n")
        (self.reports / "e2e-report-linux-2.tsv").write_text(
            "skip\t60_commented.sh\t0\tmissing capability: llvm\n"
            "skip\t70_short.sh\t0\tmissing capability: llvm\n")
        r = self.run_check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("UNCOVERED: 60_commented.sh", r.stdout)
        self.assertIn("UNCOVERED: 70_short.sh", r.stdout)

    def test_a_test_that_runs_nowhere_fails(self) -> None:
        (self.root / "tests" / "e2e" / "50_nowhere.sh").write_text("# x\n# requires: llvm\n")
        (self.reports / "e2e-report-linux-2.tsv").write_text(
            "skip\t50_nowhere.sh\t0\tmissing capability: llvm\n")
        r = self.run_check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("UNCOVERED: 50_nowhere.sh: linux: missing capability: llvm", r.stdout)

    def test_an_exception_for_a_test_that_ran_fails(self) -> None:
        (self.reports / "e2e-report-macos-1.tsv").write_text("pass\t30_excused.sh\t10\t\n")
        r = self.run_check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("STALE EXCEPTION: 30_excused.sh is excused but ran on macos", r.stdout)

    def test_a_test_required_on_hosts_must_pass_on_each(self) -> None:
        (self.root / "tests" / "e2e" / "host-requirements.tsv").write_text(
            "# test\thosts\treason\n10_runs.sh\tlinux macos windows\tverified rows\n")
        (self.reports / "e2e-report-macos-1.tsv").write_text("pass\t10_runs.sh\t10\t\n")
        (self.reports / "e2e-report-windows-1.tsv").write_text(
            "skip\t10_runs.sh\t0\tmissing capability: llvm\n")
        r = self.run_check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("REQUIRED ON HOST: 10_runs.sh did not pass on windows (windows: missing capability: llvm)",
                      r.stdout)
        self.assertNotIn("did not pass on linux", r.stdout)
        self.assertNotIn("did not pass on macos", r.stdout)
        (self.reports / "e2e-report-windows-1.tsv").write_text("pass\t10_runs.sh\t10\t\n")
        self.assertEqual(self.run_check().returncode, 0)

    def test_a_host_requirement_for_a_missing_test_fails(self) -> None:
        (self.root / "tests" / "e2e" / "host-requirements.tsv").write_text("99_gone.sh\tlinux\tx\n")
        r = self.run_check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("STALE EXCEPTION: 99_gone.sh is required on hosts but does not exist", r.stdout)

    def test_an_exception_for_a_missing_test_fails(self) -> None:
        with (self.root / "tests" / "e2e" / "coverage-exceptions.tsv").open("a") as f:
            f.write("99_gone.sh\tremoved\n")
        r = self.run_check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("STALE EXCEPTION: 99_gone.sh is excused but does not exist", r.stdout)

    def test_no_report_says_nothing_and_fails(self) -> None:
        for p in self.reports.iterdir():
            p.unlink()
        self.assertEqual(self.run_check().returncode, 1)

    def test_merged_timings_are_written_per_host(self) -> None:
        out = self.root / "timings"
        self.assertEqual(self.run_check("--timings-out", str(out)).returncode, 0)
        self.assertEqual((out / "linux.tsv").read_text(), "10_runs.sh\t1200\n")


if __name__ == "__main__":
    unittest.main()
