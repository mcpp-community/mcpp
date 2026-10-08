#!/usr/bin/env python3
"""Fixture tests for .github/tools/check_workflow_assertions.py (WS7).

Each rule is shown to fire on the shape it exists for and to stay silent on
the shapes that are correct, so a lint that stopped reading a file, or read a
step's block wrongly, fails here rather than passing every workflow.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".github" / "tools" / "check_workflow_assertions.py"

spec = importlib.util.spec_from_file_location("check_workflow_assertions", SCRIPT)
lint = importlib.util.module_from_spec(spec)
sys.modules["check_workflow_assertions"] = lint
spec.loader.exec_module(lint)


def problems_for(text: str) -> list[str]:
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "wf.yml"
        p.write_text(text, encoding="utf-8")
        return lint.check([p], check_open=False)


# The step of #729, verbatim in shape: a build piped into tee, then a grep.
TEE_NO_PIPEFAIL = """\
name: ci
on: push
jobs:
  toolchain:
    name: toolchain
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: "Toolchain: LLVM — build mcpp"
        run: |
          "$MCPP" clean
          "$MCPP" build 2>&1 | tee build.log; grep -q "Resolved llvm@20.1.7" build.log
"""


class W1Pipefail(unittest.TestCase):
    def test_a_tee_without_pipefail_is_refused(self) -> None:
        found = problems_for(TEE_NO_PIPEFAIL)
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].startswith("W1 "), found)

    def test_set_o_pipefail_in_the_block_is_accepted(self) -> None:
        text = TEE_NO_PIPEFAIL.replace('          "$MCPP" clean\n',
                                       '          set -o pipefail\n          "$MCPP" clean\n')
        self.assertEqual(problems_for(text), [])

    def test_an_explicit_bash_shell_is_accepted(self) -> None:
        # GitHub runs `shell: bash` as `bash --noprofile --norc -eo pipefail {0}`.
        text = TEE_NO_PIPEFAIL.replace('        run: |\n', '        shell: bash\n        run: |\n')
        self.assertEqual(problems_for(text), [])

    def test_a_job_default_shell_is_accepted(self) -> None:
        text = TEE_NO_PIPEFAIL.replace("    runs-on: ubuntu-24.04\n",
                                       "    runs-on: ubuntu-24.04\n    defaults:\n      run:\n        shell: bash\n")
        self.assertEqual(problems_for(text), [])

    def test_reading_pipestatus_is_accepted(self) -> None:
        text = TEE_NO_PIPEFAIL.replace(
            '"$MCPP" build 2>&1 | tee build.log; grep -q "Resolved llvm@20.1.7" build.log',
            '"$MCPP" build 2>&1 | tee build.log\n          rc=${PIPESTATUS[0]}\n          [ "$rc" = 0 ]')
        self.assertEqual(problems_for(text), [])

    def test_a_powershell_step_is_not_read_as_bash(self) -> None:
        text = TEE_NO_PIPEFAIL.replace('        run: |\n', '        shell: pwsh\n        run: |\n')
        self.assertEqual(problems_for(text), [])


class W2DiscardedStatus(unittest.TestCase):
    def test_a_build_step_that_discards_the_build_and_greps_is_refused(self) -> None:
        text = """\
jobs:
  j:
    name: j
    steps:
      - name: build the thing
        shell: bash
        run: |
          make all > build.log 2>&1 || true
          grep -q "done" build.log
"""
        found = problems_for(text)
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].startswith("W2 "), found)

    def test_a_step_named_for_something_else_is_not_read_as_a_build(self) -> None:
        text = """\
jobs:
  j:
    name: j
    steps:
      - name: Inspect the payload
        shell: bash
        run: |
          ls payload || true
          grep -q "x" notes.txt
"""
        self.assertEqual(problems_for(text), [])


KNOWN_RED = """\
jobs:
  mac:
    name: macOS (${{ matrix.image }})
    strategy:
      matrix:
        include:
          - image: macos-15
            known_red: ''
          - image: xcode-27
            known_red: '#669'
    runs-on: ${{ matrix.image }}
    continue-on-error: ${{ matrix.known_red != '' }}
    steps:
      - name: check
        run: echo ok
"""


class W3KnownRed(unittest.TestCase):
    def test_a_known_red_leg_with_its_issue_is_accepted(self) -> None:
        self.assertEqual(problems_for(KNOWN_RED), [])

    def test_a_job_allowed_to_fail_without_an_issue_is_refused(self) -> None:
        text = KNOWN_RED.replace("            known_red: '#669'\n", "            known_red: 'yes'\n")
        found = problems_for(text)
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].startswith("W3 "), found)

    def test_continue_on_error_false_is_not_known_red(self) -> None:
        text = KNOWN_RED.replace("${{ matrix.known_red != '' }}", "false").replace(
            "            known_red: '#669'\n", "            known_red: ''\n")
        self.assertEqual(problems_for(text), [])


class TheRepository(unittest.TestCase):
    def test_every_workflow_of_this_repository_is_read_and_passes(self) -> None:
        workflows = sorted((REPO_ROOT / ".github" / "workflows").glob("*.yml"))
        self.assertGreater(len(workflows), 10)
        # The denominator: the lint parses steps and run blocks from every
        # file, so a parser that read nothing cannot pass by finding nothing.
        steps = sum(len(j.steps) for p in workflows for j in lint.parse(p).jobs)
        runs = sum(1 for p in workflows for j in lint.parse(p).jobs for s in j.steps if s.run)
        self.assertGreater(steps, 200)
        self.assertGreater(runs, 150)
        known = [j.key for p in workflows for j in lint.parse(p).jobs if j.continue_on_error]
        # The four known-red legs (#669: ci-macos, ci-macos-e2e and the two
        # fresh-install macOS jobs) left the mechanism with the LLVM 23.1.3
        # line move, which removed their external cause. Zero is the state the
        # assertion table requires; the mechanism itself stays covered by the
        # fixture tests above.
        self.assertEqual(len(known), 0, known)
        self.assertEqual(lint.check(workflows, check_open=False), [])


class W4ShardCoverage(unittest.TestCase):
    def test_missing_shard_is_rejected(self):
        self.assertTrue(lint.incomplete_shards("- image: xcode-27\nshard: 1\nshards: 2\n"))

    def test_full_image_is_accepted(self):
        self.assertEqual(lint.incomplete_shards("- image: xcode-27\nshard: 1\nshards: 2\n- image: xcode-27\nshard: 2\nshards: 2\n"), [])

    def test_duplicate_shard_is_rejected(self):
        self.assertTrue(lint.incomplete_shards("- image: xcode-27\nshard: 1\nshards: 2\n- image: xcode-27\nshard: 1\nshards: 2\n"))


class W5JobRunnerContext(unittest.TestCase):
    def test_job_env_runner_is_rejected_before_startup(self):
        text = 'jobs:\n  native:\n    env:\n      REPORT: ${{ runner.temp }}/report\n    steps:\n      - run: true\n'
        found = problems_for(text)
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].startswith('W5 '), found)

    def test_step_runner_and_job_github_context_are_accepted(self):
        text = 'jobs:\n  native:\n    env:\n      SOURCE: ${{ github.workspace }}\n    steps:\n      - run: true\n        env:\n          REPORT: ${{ runner.temp }}/report\n'
        self.assertEqual(problems_for(text), [])

    def test_expression_string_does_not_name_a_context(self):
        text = "jobs:\n  native:\n    env:\n      LABEL: ${{ 'runner.temp' }}\n    steps:\n      - run: true\n"
        self.assertEqual(problems_for(text), [])


if __name__ == "__main__":
    unittest.main()
