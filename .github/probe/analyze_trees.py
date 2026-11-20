#!/usr/bin/env python3
"""The probe's analysis (temporary; this branch is never merged).

Reads what mcpp-timed.sh recorded under PROBE_DIR and prints, as Markdown:
the machine; every invocation of the trees suite's case 955 (and the slowest
others) with its wall time, the CPU its process tree used and their ratio;
per invocation, the edges each build ran (from the ninja logs, by configuration);
and the machine's load while each long invocation ran (vmstat).
"""
import collections
import datetime
import json
import os
import sys


def load_invocations(d):
    path = os.path.join(d, "invocations.jsonl")
    if not os.path.exists(path):
        return []
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    out.sort(key=lambda r: r["start"])
    return out


def read_ninja_log(path):
    entries = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 4:
                continue
            try:
                entries.append((int(parts[0]), int(parts[1]), parts[3]))
            except ValueError:
                continue
    return entries


def ninja_by_invocation(d, invocations):
    """{n: {log name: [new entries]}}: each log's entries minus those of the same
    log (same directory, same path) copied after the previous invocation there."""
    root = os.path.join(d, "ninja")
    seen = {}  # (cwd, log name) -> number of entries already attributed
    result = {}
    for inv in invocations:
        n = inv["n"]
        nd = os.path.join(root, n)
        if not os.path.isdir(nd):
            continue
        per = {}
        for name in sorted(os.listdir(nd)):
            entries = read_ninja_log(os.path.join(nd, name))
            key = (inv["cwd"], name)
            before = seen.get(key, 0)
            if len(entries) < before:  # the log was rewritten (recompacted)
                before = 0
            new = entries[before:]
            seen[key] = len(entries)
            if new:
                per[name.replace("%", "/")] = new
        if per:
            result[n] = per
    return result


def read_vmstat(d):
    """[(epoch, r, b, us, sy, id, wa)] from `vmstat -t 5` (the timestamp is UTC on the runner)."""
    path = os.path.join(d, "vmstat.txt")
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 19 or not parts[0].isdigit():
                continue
            try:
                ts = datetime.datetime.strptime(parts[-2] + " " + parts[-1], "%Y-%m-%d %H:%M:%S")
                epoch = ts.replace(tzinfo=datetime.timezone.utc).timestamp()
                r, b = int(parts[0]), int(parts[1])
                us, sy, idle, wa = int(parts[12]), int(parts[13]), int(parts[14]), int(parts[15])
                rows.append((epoch, r, b, us, sy, idle, wa))
            except (ValueError, IndexError):
                continue
    return rows


def load_during(vm, start, end):
    window = [row for row in vm if start <= row[0] <= end]
    if not window:
        return "-"
    rq = sum(r[1] for r in window) / len(window)
    busy = sum(r[3] + r[4] for r in window) / len(window)
    return f"run queue {rq:.1f} (peak {max(r[1] for r in window)}), CPU busy {busy:.0f}%"


def short(argv, limit=90):
    s = " ".join(argv)
    return s if len(s) <= limit else s[: limit - 3] + "..."


def is_trees_955(inv):
    # The case's package is `uni` (tests/e2e/openkal/trees.cpp, case 955), and each of
    # its commands is a `pack` of it.
    return inv["argv"][:1] == ["pack"] and inv.get("pkg") == "uni"


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("PROBE_DIR", ".")
    label = sys.argv[2] if len(sys.argv) > 2 else ""
    inv = load_invocations(d)
    vm = read_vmstat(d)
    print(f"## The probe: {label}\n")
    mpath = os.path.join(d, "machine.txt")
    if os.path.exists(mpath):
        with open(mpath, encoding="utf-8", errors="replace") as f:
            facts = [l.strip() for l in f if l.strip()]
        keep = [l for l in facts if l.split(":")[0].strip() in ("Model name", "CPU(s)", "Thread(s) per core")
                or l.startswith("Mem:") or l.isdigit()]
        print("Machine: " + "; ".join(keep) + "\n")
    if not inv:
        print("No invocation was recorded.\n")
        return
    span = inv[-1]["end"] - inv[0]["start"]
    print(f"{len(inv)} invocations of mcpp over {span:.0f} s.\n")

    picked = [r for r in inv if is_trees_955(r)]
    print("### The commands of trees/955 (pack of `uni`)\n")
    print("| pass | step | wall s | CPU s (user+sys) | CPU/wall | exit | load while it ran | command |")
    print("|---|---|---:|---:|---:|---:|---|---|")
    steps = collections.Counter()
    for r in picked:
        a = " ".join(r["argv"])
        step = ("E" if "--systems" in a else "C" if "self-contained" in a else
                "D" if "-openkal" in a and "--target openkal" not in a else "A")
        steps[(r["pass"], step)] += 1
        cpu = r["user"] + r["sys"]
        ratio = cpu / r["wall"] if r["wall"] > 0 else 0
        print(f"| {r['pass'] or '-'} | {step} | {r['wall']:.1f} | {cpu:.1f} | {ratio:.2f} | {r['rc']} | "
              f"{load_during(vm, r['start'], r['end'])} | `{short(r['argv'])}` |")
    print()

    others = sorted((r for r in inv if not is_trees_955(r)), key=lambda r: -r["wall"])[:15]
    if others:
        print("### The slowest other invocations\n")
        print("| wall s | CPU s | CPU/wall | exit | command | directory |")
        print("|---:|---:|---:|---:|---|---|")
        for r in others:
            cpu = r["user"] + r["sys"]
            print(f"| {r['wall']:.1f} | {cpu:.1f} | {cpu / r['wall'] if r['wall'] else 0:.2f} | {r['rc']} | "
                  f"`{short(r['argv'], 60)}` | `{r['cwd'][-50:]}` |")
        print()

    nj = ninja_by_invocation(d, inv)
    print("### The builds inside each trees/955 command (ninja logs, by configuration)\n")
    for r in picked:
        per = nj.get(r["n"], {})
        if not per:
            continue
        print(f"**{r['pass'] or '-'} `{short(r['argv'], 70)}`** ({r['wall']:.1f} s)\n")
        print("| build directory | edges | edge time s (sum) | span s | slowest edges |")
        print("|---|---:|---:|---:|---|")
        for name, entries in sorted(per.items(), key=lambda kv: -sum(e[1] - e[0] for e in kv[1])):
            total = sum(e[1] - e[0] for e in entries) / 1000
            spans = (max(e[1] for e in entries) - min(e[0] for e in entries)) / 1000
            slow = sorted(entries, key=lambda e: -(e[1] - e[0]))[:3]
            slow_s = ", ".join(f"{os.path.basename(e[2])} {(e[1] - e[0]) / 1000:.0f}s" for e in slow)
            print(f"| `{name[-70:]}` | {len(entries)} | {total:.0f} | {spans:.0f} | {slow_s} |")
        print()


if __name__ == "__main__":
    main()
