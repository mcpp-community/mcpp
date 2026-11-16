#!/usr/bin/env python3
"""Every e2e test runs somewhere, or says why it cannot (rule R5).

WHY THIS EXISTS

`run_all.sh` exits 0 on a skip, and a skip line reads the same whether the
host legitimately lacks a capability or the runner was set up wrong. Measured
on 2026-10-01, before this check: 24 of 564 tests ran on no host and were named
by no workflow. Seven declared `llvm`, which no line granted; three declared
`musl`, whose probe named a release (15.1.0) the runners no longer installed;
seven declared `mingw-cross`, which no shard installed. Every one of those runs
was green.

WHAT IT CHECKS

It reads the per-test reports the shards write (`E2E_REPORT` of run_all.sh, one
`<status>\\t<test>\\t<ms>\\t<detail>` line per test) and decides, for every test
under tests/e2e:

  ran        some report says pass, fail or timeout;
  unaffected no report ran it, and a run narrowed to a set (`E2E_SET`, the
             `pr` set of a pull request) says the change does not affect it:
             the full runs of main are its coverage, and this run's claim is
             about the tests the change reaches;
  job        no report ran it, but a workflow names it outside a comment, as a
             whole token: by file name, by name without `.sh`, or by its number
             as an `E2E_ONLY` pattern such as `239_*.sh`. A dedicated job runs it
             and asserts its result itself. A name in a comment does not count,
             and neither does a longer name that contains it;
  excused    tests/e2e/coverage-exceptions.tsv lists it with the reason no hosted
             runner can run it;
  owed       tests/e2e/coverage-debt.tsv lists it: it skips itself on every
             host that starts it (a script leaves with 77 after a `SKIP:` line
             when it can hold nothing on a machine, and the run reports it as a
             skip). The list is a debt, and it only shrinks: each entry names
             what a host lacks, and is removed when a host gains it;
  uncovered  none of these. The check fails.

An exception or a debt entry for a test that ran, or for a test that does not
exist, also fails the check, so that neither list can outlive the reason it
records.

Some claims need a test to pass on particular hosts, not on some host: docs/21
calls a target row `verified` when CI builds and runs its artefact, and the
`*-openkal` rows claim that for every system. tests/e2e/host-requirements.tsv
lists such a test with the hosts its result must come from; a listed test that
did not pass on one of them, or that does not exist, fails the check; one a
set left out on that host as unaffected meets it for that run.

With --timings-out DIR it writes `<host>.tsv`, the measured duration of every
test that ran, merged across that host's shards, in the format run_all.sh reads
from E2E_TIMINGS (a run without mtest; CI's shards divide by mtest's history). A report
file is named `e2e-report-<host>-<shard>.tsv`.

Usage:
    check_e2e_coverage.py --reports DIR [--root DIR] [--timings-out DIR]
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

RAN = {"pass", "fail", "timeout"}
REPORT_NAME = re.compile(r"e2e-report-(?P<host>[a-z0-9-]+?)-(?P<shard>\d+)\.tsv$")


def read_reports(directory: Path):
    """Yield (host, shard, status, test, ms, detail) for every report line."""
    for path in sorted(directory.rglob("e2e-report-*.tsv")):
        m = REPORT_NAME.search(path.name)
        if not m:
            continue
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            ms = int(parts[2]) if parts[2].isdigit() else 0
            yield m["host"], int(m["shard"]), parts[0], parts[1], ms, parts[3] if len(parts) > 3 else ""


def read_host_requirements(path: Path) -> dict[str, set[str]]:
    """`<test>\t<host> <host> ...\t<reason>` lines: the hosts a test must pass on."""
    out: dict[str, set[str]] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        out[parts[0].strip()] = set(parts[1].split()) if len(parts) > 1 else set()
    return out


def read_exceptions(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        test, _, reason = line.partition("\t")
        out[test.strip()] = reason.strip()
    return out


TOKEN = re.compile(r"(?<![A-Za-z0-9_.-])(\d+[a-z]?_(?:\*|[A-Za-z0-9_]+))(?:\.sh)?(?![A-Za-z0-9_])")


def workflow_tokens(texts: list[str]) -> set[str]:
    """Every test name or `<number>_*` pattern named outside a comment."""
    tokens: set[str] = set()
    for text in texts:
        for line in text.splitlines():
            code = line.split("#", 1)[0] if line.lstrip().startswith("#") else line
            if not code.strip():
                continue
            tokens.update(m.group(1) for m in TOKEN.finditer(code))
    return tokens


def named_by_a_workflow(test: str, tokens: set[str]) -> bool:
    stem = test[:-3]
    number = stem.split("_", 1)[0]
    return stem in tokens or f"{number}_*" in tokens


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reports", required=True)
    parser.add_argument("--root", default=".")
    parser.add_argument("--timings-out")
    args = parser.parse_args()
    root = Path(args.root).resolve()

    tests = sorted(p.name for p in (root / "tests" / "e2e").glob("[0-9]*.sh"))
    workflows = workflow_tokens([p.read_text(encoding="utf-8")
                                 for p in sorted((root / ".github" / "workflows").glob("*.yml"))])
    exceptions = read_exceptions(root / "tests" / "e2e" / "coverage-exceptions.tsv")
    debt = read_exceptions(root / "tests" / "e2e" / "coverage-debt.tsv")
    required = read_host_requirements(root / "tests" / "e2e" / "host-requirements.tsv")

    ran: dict[str, set[str]] = defaultdict(set)
    passed: dict[str, set[str]] = defaultdict(set)
    skipped: dict[str, set[str]] = defaultdict(set)
    unaffected: dict[str, set[str]] = defaultdict(set)
    shard_ms: dict[tuple[str, int], int] = defaultdict(int)
    timings: dict[str, dict[str, int]] = defaultdict(dict)
    reports = 0
    for host, shard, status, test, ms, detail in read_reports(Path(args.reports)):
        reports += 1
        if status in RAN:
            ran[test].add(host)
            if status == "pass":
                passed[test].add(host)
            shard_ms[(host, shard)] += ms
            timings[host][test] = ms
        elif status == "skip":
            skipped[test].add(f"{host}: {detail}")
        elif status == "unaffected":
            unaffected[test].add(host)
    if reports == 0:
        print(f"no report under {args.reports}: nothing can be said about coverage")
        return 1

    uncovered, stale = [], []
    counts = defaultdict(int)
    for test in tests:
        if test in ran:
            counts["ran"] += 1
            if test in exceptions:
                stale.append(f"{test} is excused but ran on {', '.join(sorted(ran[test]))}")
            if test in debt:
                stale.append(f"{test} is listed as owed but ran on {', '.join(sorted(ran[test]))}: "
                             f"remove it from coverage-debt.tsv")
        elif test in unaffected:
            counts["unaffected"] += 1
        elif named_by_a_workflow(test, workflows):
            counts["job"] += 1
        elif test in exceptions:
            counts["excused"] += 1
        elif test in debt:
            counts["owed"] += 1
        else:
            uncovered.append(f"{test}: {'; '.join(sorted(skipped[test])) or 'in no report'}")
    for test in exceptions:
        if test not in tests:
            stale.append(f"{test} is excused but does not exist")
    for test in debt:
        if test not in tests:
            stale.append(f"{test} is listed as owed but does not exist")
    unmet = []
    for test, hosts in sorted(required.items()):
        if test not in tests:
            stale.append(f"{test} is required on hosts but does not exist")
            continue
        for host in sorted(hosts - passed[test] - unaffected[test]):
            why = [d for d in skipped[test] if d.startswith(f"{host}: ")]
            unmet.append(f"{test} did not pass on {host}"
                         f"{' (' + '; '.join(why) + ')' if why else (' (it ran and did not pass)' if host in ran[test] else ' (no report of it there)')}")

    print(f"{len(tests)} tests: {counts['ran']} ran on a shard, {counts['unaffected']} unaffected by "
          f"the change, {counts['job']} run by a dedicated job, {counts['excused']} excused, "
          f"{counts['owed']} owed, {len(uncovered)} uncovered")
    for (host, shard), ms in sorted(shard_ms.items()):
        print(f"  {host} shard {shard}: {ms / 60000:.1f} min of tests")
    for line in uncovered:
        print(f"UNCOVERED: {line}")
    for line in stale:
        print(f"STALE EXCEPTION: {line}")
    for line in unmet:
        print(f"REQUIRED ON HOST: {line}")

    if args.timings_out:
        out = Path(args.timings_out)
        out.mkdir(parents=True, exist_ok=True)
        for host, table in timings.items():
            (out / f"{host}.tsv").write_text(
                "".join(f"{t}\t{ms}\n" for t, ms in sorted(table.items())), encoding="utf-8")
    return 1 if uncovered or stale or unmet else 0


if __name__ == "__main__":
    sys.exit(main())
