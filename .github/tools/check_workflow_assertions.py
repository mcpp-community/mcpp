#!/usr/bin/env python3
"""A CI step asserts what its name says, or it does not exist (P6 of the
2026-09-28 ecosystem design, WS7).

WHY THIS EXISTS

#729: the step "Toolchain: LLVM -- build mcpp" ran

    "$MCPP" build 2>&1 | tee build.log; grep -q "Resolved llvm@23.1.3" build.log

A pipeline's status is its last command's. Under GitHub's default shell for a
`run:` block with no `shell:` key (`bash -e {0}`, no pipefail) the build's
failure disappeared into `tee`, the step asserted only that a toolchain had
been resolved, and it was green on `main` while the build failed. Three more
steps had the same shape. This check reads every workflow and refuses the
shape, so the next one is caught when it is written.

THE RULES

  W1  A `run:` block that pipes a command into `tee` must run with pipefail:
      the step's shell is `bash` stated explicitly (GitHub runs that as
      `bash --noprofile --norc -eo pipefail {0}`), or a job or workflow
      `defaults.run.shell` says so, or the block itself runs
      `set -o pipefail` (or `set -eo pipefail`, `set -euo pipefail`) before
      the pipe. A block with no pipe into `tee` is not affected.
  W2  A step whose name says it builds, tests or installs, and whose last
      statement is a text match (`grep`), must not discard that verb's exit
      status with `|| true` or `|| :` -- otherwise its only assertion is the
      text match, which is what #729 was.
  W3  A job that is allowed to fail (`continue-on-error: true`, or an
      expression that makes one matrix leg so) is a known-red job, and its
      name must carry the issue that tracks it (`#<n>`). With `--check-open`
      (and `GH_TOKEN`), every such issue must be open: a job leaves the list
      when its issue closes.

Where it stands beside `tools/lint-ci-assertions.sh`: that script WARNS about
where an assertion is placed (a matrix row, an emptiness check, a job with no
emulator), because those rules have real false positives. These three rules
have none found in this repository, so they are a gate.

The parser is line-based and fitted to this repository's workflow layout
(two-space indentation, `jobs:` at column 0, steps as `- name:` items). It
reads no YAML library, because the checks job has none installed.

Usage:
    python3 .github/tools/check_workflow_assertions.py [--check-open] [workflow.yml ...]
Without arguments it reads .github/workflows/*.yml. Exit status 1 when a rule
is broken, 0 otherwise.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

PIPEFAIL_RE = re.compile(r"\bset\s+-[a-z]*o\s+pipefail\b|\bset\s+-o\s+pipefail\b|\bset\s+-[a-z]*e[a-z]*o\s+pipefail\b")
TEE_PIPE_RE = re.compile(r"\|\s*tee\b")
VERB_RE = re.compile(r"\b(build|builds|test|tests|install|installs)\b", re.IGNORECASE)
DISCARD_RE = re.compile(r"\|\|\s*(true|:)\s*(;|$)")
ISSUE_RE = re.compile(r"#(\d+)")


@dataclass
class Step:
    name: str = ""
    shell: str = ""
    run: str = ""
    line: int = 0


@dataclass
class Job:
    key: str
    name: str = ""
    shell: str = ""
    continue_on_error: str = ""
    matrix_text: str = ""
    steps: list[Step] = field(default_factory=list)
    line: int = 0


@dataclass
class Workflow:
    path: Path
    shell: str = ""
    jobs: list[Job] = field(default_factory=list)


def indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def scalar(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def read_block(lines: list[str], i: int, parent_indent: int) -> tuple[str, int]:
    """The literal block that follows a `key: |` (or `>`) line at index i."""
    out: list[str] = []
    j = i + 1
    block_indent = None
    while j < len(lines):
        raw = lines[j]
        if raw.strip() == "":
            out.append("")
            j += 1
            continue
        ind = indent_of(raw)
        if ind <= parent_indent:
            break
        if block_indent is None:
            block_indent = ind
        out.append(raw[block_indent:] if ind >= block_indent else raw.strip())
        j += 1
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out), j


def parse(path: Path) -> Workflow:
    wf = Workflow(path)
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    job: Job | None = None
    step: Step | None = None
    in_jobs = False
    section = ""          # within a job: "", "steps", "strategy", "defaults"
    top_defaults = False
    while i < len(lines):
        raw = lines[i]
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            i += 1
            continue
        ind = indent_of(raw)
        if ind == 0:
            in_jobs = stripped == "jobs:"
            top_defaults = stripped == "defaults:"
            job = None
            step = None
            i += 1
            continue
        if top_defaults and stripped.startswith("shell:"):
            wf.shell = scalar(stripped.split(":", 1)[1])
        if not in_jobs:
            i += 1
            continue
        if ind == 2 and stripped.endswith(":"):
            job = Job(key=stripped[:-1], line=i + 1)
            wf.jobs.append(job)
            step = None
            section = ""
            i += 1
            continue
        if job is None:
            i += 1
            continue
        if ind == 4:
            key, _, value = stripped.partition(":")
            section = key
            if key == "name":
                job.name = scalar(value)
            elif key == "continue-on-error":
                job.continue_on_error = scalar(value)
            i += 1
            continue
        if section == "strategy":
            job.matrix_text += stripped + "\n"
            i += 1
            continue
        if section == "defaults" and stripped.startswith("shell:"):
            job.shell = scalar(stripped.split(":", 1)[1])
            i += 1
            continue
        if section != "steps":
            i += 1
            continue
        if ind == 6 and stripped.startswith("- "):
            step = Step(line=i + 1)
            job.steps.append(step)
            stripped = stripped[2:].strip()
            ind = 8
        if step is None:
            i += 1
            continue
        if ind == 8:
            key, _, value = stripped.partition(":")
            value = value.strip()
            if key == "name":
                step.name = scalar(value)
            elif key == "shell":
                step.shell = scalar(value)
            elif key == "run":
                if value in ("|", ">", "|-", ">-", "|+", ">+"):
                    # A step's keys sit at indent 8 whether `run:` opens the
                    # item (`- run: |`) or follows its name, so the block is
                    # everything indented deeper than 8.
                    step.run, i = read_block(lines, i, 8)
                    continue
                step.run = value
        i += 1
    return wf


def effective_shell(wf: Workflow, job: Job, step: Step) -> str:
    return step.shell or job.shell or wf.shell or ""


def has_pipefail(wf: Workflow, job: Job, step: Step) -> bool:
    if effective_shell(wf, job, step) == "bash":
        return True
    return bool(PIPEFAIL_RE.search(step.run))


def last_statement(script: str) -> str:
    stmts = [s.strip() for s in re.split(r"[;\n]", script) if s.strip() and not s.strip().startswith("#")]
    return stmts[-1] if stmts else ""


def check(workflows: list[Path], check_open: bool) -> list[str]:
    problems: list[str] = []
    known_red: list[tuple[str, str, int]] = []
    for path in workflows:
        wf = parse(path)
        for job in wf.jobs:
            for step in job.steps:
                where = f"{path}:{step.line} ({job.key} / {step.name or 'unnamed step'})"
                shell = effective_shell(wf, job, step)
                if shell in ("pwsh", "powershell", "cmd"):
                    continue
                # W1. A pipe whose next statement reads `${PIPESTATUS[0]}`
                # asserts the piped command's status itself and is accepted.
                run_lines = step.run.splitlines()
                pipe_lines = []
                for k, l in enumerate(run_lines):
                    if not TEE_PIPE_RE.search(l) or l.strip().startswith("#"):
                        continue
                    rest = l.split("|", 1)[1] if "PIPESTATUS" in l else ""
                    following = next((x for x in run_lines[k + 1:]
                                      if x.strip() and not x.strip().startswith("#")), "")
                    if "PIPESTATUS" in rest or "PIPESTATUS" in following:
                        continue
                    pipe_lines.append(l)
                if pipe_lines and not has_pipefail(wf, job, step):
                    problems.append(
                        f"W1 {where}: pipes into tee without pipefail, so the "
                        f"piped command's failure is lost: `{pipe_lines[0].strip()}`. "
                        f"State `shell: bash` on the step or `set -o pipefail` first.")
                # W2
                if step.name and VERB_RE.search(step.name) and step.run:
                    last = last_statement(step.run)
                    if last.startswith("grep"):
                        for l in step.run.splitlines():
                            if DISCARD_RE.search(l) and not l.strip().startswith("grep"):
                                problems.append(
                                    f"W2 {where}: the step is named for what it "
                                    f"{VERB_RE.search(step.name).group(1).lower()}s, discards "
                                    f"that command's status (`{l.strip()}`), and asserts "
                                    f"only a text match.")
                                break
            # W3
            coe = job.continue_on_error
            if coe and coe.lower() != "false":
                issue = ISSUE_RE.search(job.name) or ISSUE_RE.search(job.matrix_text)
                if not issue:
                    problems.append(
                        f"W3 {path}:{job.line} ({job.key}): allowed to fail "
                        f"(`continue-on-error: {coe}`), but its name names no "
                        f"issue that tracks it.")
                else:
                    for n in ISSUE_RE.findall(job.name + "\n" + job.matrix_text):
                        known_red.append((str(path), n, job.line))
    if check_open and known_red:
        seen: dict[str, str] = {}
        for path, n, line in known_red:
            if n not in seen:
                seen[n] = issue_state(n)
            state = seen[n]
            if state != "OPEN":
                problems.append(
                    f"W3 {path}:{line}: a known-red job names #{n}, which is "
                    f"{state.lower() if state else 'unreadable'}; a job leaves the "
                    f"known-red list when its issue closes.")
    return problems


def issue_state(n: str) -> str:
    repo = os.environ.get("GITHUB_REPOSITORY", "mcpp-community/mcpp")
    try:
        out = subprocess.run(
            ["gh", "issue", "view", n, "--repo", repo, "--json", "state"],
            capture_output=True, text=True, timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    if out.returncode != 0:
        return ""
    try:
        return json.loads(out.stdout).get("state", "")
    except json.JSONDecodeError:
        return ""


def main(argv: list[str]) -> int:
    check_open = "--check-open" in argv
    paths = [Path(a) for a in argv if not a.startswith("--")]
    if not paths:
        paths = sorted(Path(".github/workflows").glob("*.yml"))
    if not paths:
        print("FAIL: no workflow files to read", file=sys.stderr)
        return 1
    problems = check(paths, check_open)
    for p in problems:
        print(p)
    print(f"{len(paths)} workflow(s) read, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
