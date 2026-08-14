#!/usr/bin/env bash
# requires: python3
# 233_bench_matrix.sh — bench/matrix.json is the ONE place the benchmark matrix
# is written down, and this checks that it stays that way.
#
# The failure this prevents is not a crash. It is a matrix that exists twice —
# once as data and once hard-coded in the workflow — and drifts, because both
# copies keep looking right. The same shape has already cost this repository
# real time elsewhere ("同一决策两处推导"), and a benchmark is the worst place
# for it: the numbers still come out, they are just of something else.
#
# Four things are asserted, and each names a different way of getting it wrong:
#   1. the file parses and every cell draws its coordinates from `axes`
#      — a typo'd toolchain plans a job that installs nothing;
#   2. every axis VALUE is one the harness actually accepts
#      — the spec is only worth something if it describes the real program;
#   3. every excluded cell carries a reason
#      — "not measured" must not quietly become "not applicable";
#   4. the workflow reads the file instead of repeating it.
set -e

# ⚠️ EVERY python read below MUST name its encoding, and this is what enforces it.
#
# `open()`, `read_text()` and `subprocess(text=True)` decode with the LOCALE
# encoding, which is UTF-8 on the Linux and macOS runners and cp1252 on the
# Windows one. Every file this test reads — matrix.json, the engine adapters,
# the READMEs — contains non-ASCII, so on Windows the reads either raise
#
#     UnicodeDecodeError: 'charmap' codec can't decode byte 0x8f in position 3037
#
# or, for the bytes cp1252 does happen to map, silently produce mojibake: the
# regex then matches nothing and the guard reports success while guarding
# nothing. That is the same "failure looks like success" shape this whole test
# exists to catch, so it must not be the test's own failure mode.
#
# §1 read matrix.json without an encoding for a while and stayed green purely
# because its non-ASCII bytes missed cp1252's five undefined ones; §7 hit 0x8f
# and turned the whole Windows e2e job red. Both are the same defect.
#
# These two variables turn an unspecified encoding into a hard error, so the
# next one fails on the FIRST machine that runs it rather than only on Windows.
# Ignored by Python < 3.10, which predates EncodingWarning.
export PYTHONWARNDEFAULTENCODING=1
export PYTHONWARNINGS=error::EncodingWarning

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MATRIX="$ROOT/bench/matrix.json"
RUNNER="$ROOT/bench/run-standard.sh"
SPEC="$ROOT/bench/SPEC.md"

[ -f "$MATRIX" ]   || { echo "FAIL: bench/matrix.json is missing"; exit 1; }
[ -f "$SPEC" ]     || { echo "FAIL: bench/SPEC.md is missing"; exit 1; }
[ -f "$RUNNER" ] || { echo "FAIL: bench/run-standard.sh is missing"; exit 1; }

# ── 1..3: the data ─────────────────────────────────────────────────────────
# ROOT is passed in: this python runs from stdin, so sys.argv[0] is "-" and the
# repository cannot be derived from it.
python3 - "$MATRIX" "$ROOT" <<'PY'
import json, os, re, sys

m = json.load(open(sys.argv[1], encoding="utf-8"))
axes = m["axes"]
fail = []

def check_list(where, field, value, axis):
    # `mcpp[schedule=on]` is ONE engine with an option list, not a second engine.
    # Splitting on commas alone would also tear `a[x=1,y=2]` in half, so brackets
    # are consumed before the split rather than after it.
    items = re.findall(r"[^,\[\]]+(?:\[[^\]]*\])?", value) if "[" in value else value.split(",")
    for v in items:
        v = v.strip()
        if not v:
            continue
        # `native` is the real-project variant: a tree has exactly one form,
        # its own, so it is not a generated axis value.
        if axis == "variant" and v == "native":
            continue
        # The axis is the engine NAME; the options modify it. They are checked
        # for real by the registry (`engine_option` rejects an unknown one and
        # the whole spec with it), so repeating that list here would be a second
        # source of truth for it.
        base = v.split("[", 1)[0] if axis == "engine" else v
        if base not in axes[axis]:
            fail.append(f"{where}: {field}='{v}' is not in axes.{axis} {axes[axis]}")

seen = set()
if not m.get("cells"):
    fail.append("the matrix has no cells at all — every per-cell check below "
                "would pass by having nothing to check")
for c in m["cells"]:
    where = f"{c.get('os')}/{c.get('toolchain')}/{c.get('project')}"
    for field, axis in (("os", "os"), ("toolchain", "toolchain"), ("project", "project")):
        if c.get(field) not in axes[axis]:
            fail.append(f"{where}: {field}='{c.get(field)}' is not in axes.{axis}")
    if where in seen:
        fail.append(f"{where}: duplicated cell — two jobs would write the same report file")
    seen.add(where)
    check_list(where, "engines",   c["engines"],   "engine")
    check_list(where, "variants",  c["variants"],  "variant")
    check_list(where, "scenarios", c["scenarios"], "scenario")

# The baseline must be an engine, and it must actually be IN every cell it is
# supposed to normalise — a ratio against an engine that never ran is not a
# ratio, and the report renders it as bare seconds.
# A cell may OVERRIDE it: the xlings arms are an mcpp-against-mcpp comparison
# because their cmake/xmake arms stop at the link, and normalising against an
# engine that never produced a binary is how a table of bare seconds gets
# published as a comparison.
base = m["baseline"]
if base not in axes["engine"]:
    fail.append(f"baseline '{base}' is not one of axes.engine")
for c in m["cells"]:
    eff = c.get("baseline", base)
    engines = [e.strip() for e in c["engines"].split(",")]
    # `mcpp` in a cell's engine list means BOTH mcpp binaries (the built one and
    # the released reference), so a reference-version baseline is satisfied by it.
    if eff in engines or (eff == m.get("reference_mcpp") and "mcpp" in engines):
        continue
    fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: baseline '{eff}' "
                f"is not among its engines — that cell would report bare seconds")

# An engine may only be waived if it is actually in the cell, and the cell must
# say why. A blanket waiver is how a permanently broken arm stops being noticed.
for c in m["cells"]:
    for w in [e.strip() for e in c.get("allow_failed", "").split(",") if e.strip()]:
        if w not in [e.strip() for e in c["engines"].split(",")]:
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: allow_failed names "
                        f"'{w}', which is not one of its engines")
    if c.get("allow_failed") and "KNOWN GAP" not in c.get("note", ""):
        fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: allow_failed without a "
                    f"'KNOWN GAP' note — a waived failure that says nothing is a hidden one")
    # ...and the note must say something about EACH waived engine by name.
    #
    # A note can only be checked for existence, never for truth, so the next
    # best thing is to stop one blanket sentence from covering two arms. It
    # already went wrong that way: the windows/clang cell waived cmake and xmake
    # under a single `import std` explanation, and by the time anyone looked
    # xmake was failing with `could not start the process` — not a language-
    # feature gap at all but a missing program on the runner, i.e. something
    # fixable, hidden behind a reason that was only ever cmake's.
    for w in [e.strip() for e in c.get("allow_failed", "").split(",") if e.strip()]:
        if w not in c.get("note", ""):
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: '{w}' is waived but the "
                        f"note never mentions it — one reason covering two arms is how a "
                        f"fixable failure hides behind an unfixable one")

# 3. Every excluded cell says why, and says something.
for x in m.get("excluded", []):
    if len(x.get("reason", "").strip()) < 20:
        fail.append(f"excluded {x.get('os')}/{x.get('toolchain')}/{x.get('project','*')}: "
                    "reason is missing or too short to be one")

# An exclusion must not also be a cell. `*` is a wildcard, `foo-*` a prefix
# wildcard (the project axis carries pinned versions, so `xlings-*` is the only
# way to say "both styles"), and an exclusion that names an `engine` scopes a
# CAVEAT to one column rather than removing the job — those legitimately coexist
# with the cell.
#
# The prefix form exists because the bare `*` is too big: written as
# `{os: windows, toolchain: clang, project: "*"}` it also claimed the
# windows/clang fixture and mcpp cells, which do run. This check caught that.
def matches(x, c, key):
    v = x.get(key)
    if v is None or v == "*":            return True
    if v.endswith("*"):                  return c[key].startswith(v[:-1])
    return v == c[key]

for x in m.get("excluded", []):
    if x.get("engine"):
        continue
    for c in m["cells"]:
        if all(matches(x, c, k) for k in ("os", "toolchain", "project")):
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']} is both a cell and excluded")

# ── The perturbation targets must EXIST ────────────────────────────────────
#
# This is the assertion the suite most needed and did not have. `--hub` pointed
# at `src/xlings.cppm` for months after that file stopped existing; the harness
# correctly reported `skipped — points at a file that does not exist`, the
# workflow correctly exited 0, and three CI jobs per run reported success having
# measured precisely nothing.
#
# It is checkable at all only because the trees are now pinned SUBMODULES rather
# than cloned from a moving branch at run time. That is most of the argument for
# pinning them.
root = sys.argv[2]
uninit = set()
for c in m["cells"]:
    if c["project"] == "fixture":
        if c.get("hub") or c.get("body"):
            fail.append(f"{c['os']}/{c['toolchain']}/fixture: hub/body are for real projects; "
                        "a generated fixture names its own targets")
        continue
    for field in ("hub", "body"):
        if not c.get(field):
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: '{field}' is required for a "
                        "real project — without it every perturbing scenario reports `skipped`")
            continue
        # EVERY workload is a pinned submodule under bench/projects/<desc>/<pin>,
        # including mcpp's own sources. There is no "this checkout" case: the
        # engine under test is the binary, the workload must not move with it.
        tree = os.path.join(root, "bench", "projects",
                            c.get("buildfiles", c["project"]), c["project"])
        # A submodule that is DECLARED but not checked out leaves an empty
        # directory, which is not the same as a missing one and must not read as
        # a broken matrix: only the bench workflow checks submodules out, so
        # every other CI job would fail this on a perfectly correct file.
        #
        # `mcpp.toml` is the marker — every workload here is an mcpp project, and
        # its absence means "not initialised" rather than "hub is wrong".
        if not os.path.isfile(os.path.join(tree, "mcpp.toml")):
            if not os.path.isdir(tree):
                fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: nothing at {tree} — "
                            "the cell names a workload that is not even declared as a submodule")
            else:
                uninit.add(c["project"])
            break
        target = os.path.join(tree, c[field])
        if not os.path.isfile(target):
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: {field}='{c[field]}' does not "
                        f"exist in the pinned tree — every scenario that perturbs it would be "
                        f"reported `skipped` and the job would still pass")

# ── No engine scratch may be tracked ───────────────────────────────────────
#
# The foreign engines write their state next to the description they are pointed
# at, and ten of xmake's cache files were committed by an over-broad `git add`.
# One of them recorded `builddir = "mcpp-2026.8.11.3/build"` — the path-doubling
# bug this suite was fixed for — in a file CI would have READ, reinstating the
# defect on every runner while the code that caused it was already gone.
#
# Checked here rather than trusted to .gitignore, because the root ignore file
# already had `/.xmake/` and it did not reach `bench/projects/` at all.
import subprocess
tracked = subprocess.run(
    ["git", "-C", root, "ls-files",
     "bench/projects/*/.xmake*", "bench/projects/*/build/*",
     "bench/projects/*/CMakeCache.txt", "bench/projects/*/bazel-*"],
    # encoding pinned, not `text=True` alone: that decodes the child's stdout
    # with the LOCALE encoding, which on a Windows runner is cp1252. A path (or
    # any UTF-8 byte) then either raises or, worse, mojibakes into something
    # that no longer matches — a guard that silently stops guarding.
    capture_output=True, encoding="utf-8").stdout.split()
if tracked:
    fail.append("engine scratch is tracked in git (machine-local state, and one of "
                f"these froze a fixed bug into CI): {tracked[:4]}"
                + (f" … and {len(tracked)-4} more" if len(tracked) > 4 else ""))

# ── The tool pins ──────────────────────────────────────────────────────────
# A pin that is absent is a tool resolved from the runner image, which is how
# the matrix ended up measuring cmake 3.31.6 against a suite that needs 4.0.
for t in ("cmake", "xmake", "bazel", "gcc", "llvm"):
    v = m.get("tools", {}).get(t, "")
    if not re.match(r"^\d+(\.\d+)+$", str(v)):
        fail.append(f"tools.{t} = {v!r} is not an exact version; an unpinned tool is a "
                    "variable the report does not record")
if not re.match(r"^\d+(\.\d+)+$", str(m.get("reference_mcpp", ""))):
    fail.append("reference_mcpp must be an exact released version — it is the old-vs-new column")

# ...and it must be the version the repository already bootstraps from.
#
# They are the same decision written in two files: `.xlings.json` says which
# released mcpp CI installs, and that installed binary IS the reference arm the
# bench compares against. Let them drift and the "old" column silently becomes
# some other release, with every ratio still looking perfectly reasonable.
xlings_pin = os.path.join(root, ".xlings.json")
if os.path.isfile(xlings_pin):
    ws = json.load(open(xlings_pin, encoding="utf-8")).get("workspace", {}).get("mcpp")
    if ws and ws != m.get("reference_mcpp"):
        fail.append(f"reference_mcpp={m.get('reference_mcpp')} but .xlings.json bootstraps "
                    f"mcpp {ws} — the reference arm IS the bootstrapped binary, so these "
                    f"two must agree or the old-vs-new column compares the wrong release")

# ...and so are the COMPILER pins, for the same reason and with a worse failure.
#
# matrix.json's `tools.gcc` / `tools.llvm` decide which toolchain CI INSTALLS.
# bench/src/toolchain.cppm's kGcc / kLlvm decide which payload path the harness
# HANDS EVERY ENGINE via `--compiler payload:*`. Those are one decision written
# in two files — matrix.json's own `_compiler_note` says as much — and nothing
# made them agree.
#
# Drift is silent in the direction that matters: xlings installs the version
# from matrix.json, the harness asks for the payload directory of the version
# from toolchain.cppm, and that directory is simply not there. Every cell then
# fails for a reason that names a path, not a pin. Checked here because this is
# already the file that cross-checks `reference_mcpp` against `.xlings.json`.
tc_src = os.path.join(root, "bench/src/toolchain.cppm")
if os.path.isfile(tc_src):
    tc = open(tc_src, encoding="utf-8").read()
    for key, const in (("gcc", "kGcc"), ("llvm", "kLlvm"), ("llvm_windows", "kLlvmWindows")):
        # `kLlvm` is a prefix of `kLlvmWindows`, so anchor on the whole name.
        mm = re.search(rf"\b{const}\b\s*=\s*\"([^\"]+)\"", tc)
        if not mm:
            fail.append(f"bench/src/toolchain.cppm no longer defines {const} — this check "
                        f"cannot compare the pins and must not pass silently")
        elif mm.group(1) != str(m.get("tools", {}).get(key, "")):
            fail.append(f"tools.{key}={m.get('tools', {}).get(key)!r} but toolchain.cppm's "
                        f"{const} is {mm.group(1)!r} — CI installs one and the harness hands "
                        f"every engine the other; the cells fail naming a missing path")

# The READMEs open with a "what is pinned" table whose whole claim is that those
# are the versions the numbers were taken with. It is prose, so nothing made it
# follow `matrix.json` — and it did not: the pin moved to cmake 4.4.2 (a version
# whose `import std` gate is a DIFFERENT UUID) while both tables still said
# 4.0.2, in the one section a reader consults to decide whether to trust the
# data. Rows only; the surrounding prose discusses older versions on purpose.
for doc in ("bench/README.md", "bench/README.zh-CN.md"):
    path = os.path.join(root, doc)
    if not os.path.isfile(path):
        continue
    text = open(path, encoding="utf-8").read()
    for tool in ("cmake", "xmake", "bazel"):
        want = str(m.get("tools", {}).get(tool, ""))
        rows = re.findall(rf"^\|\s*{tool}\s*\|\s*\*\*([^*]+)\*\*\s*\|", text, re.M)
        if not rows:
            fail.append(f"{doc}: no pinned-version row for {tool} — this check "
                        f"cannot compare anything and must not pass silently")
        for got in rows:
            if got.strip() != want:
                fail.append(f"{doc}: the pinned table says {tool} {got.strip()} but "
                            f"matrix.json pins {want} — that table is what a reader "
                            f"uses to decide whether to trust the numbers")

if fail:
    print("FAIL: bench/matrix.json")
    for f in fail:
        print("  " + f)
    raise SystemExit(1)
print(f"matrix: {len(m['cells'])} cells, {len(m.get('excluded', []))} documented exclusions, "
      f"baseline={base}, tool pins {m['tools']['cmake']}/{m['tools']['xmake']}/{m['tools']['bazel']}")
if uninit:
    # Loud, and named. A silent skip here would mean the check that catches a
    # stale `hub` never actually runs anywhere, which is how it got missed in
    # the first place. The bench workflow checks submodules out and runs this
    # test, so the assertion does execute on every change to the suite.
    print(f"  NOTE: hub/body existence NOT checked for {', '.join(sorted(uninit))} "
          f"— submodule(s) not checked out here (`git submodule update --init`)")
PY

# ── 2: the axis values are ones the harness accepts ────────────────────────
# Read out of the harness's own source, not a second list here — the whole
# point of this test is that there is no second list.
python3 - "$MATRIX" "$ROOT/bench/src/spec.cppm" "$ROOT/bench/src/registry.cppm" <<'PY'
import json, os, re, sys

# A module is its INTERFACE PLUS ITS IMPLEMENTATION UNIT. The suite writes
# declarations in `<m>.cppm` and definitions in `<m>.cpp`, so reading only the
# `.cppm` finds the declaration of `make_engine` and none of the engine names
# inside it — which is exactly how this check started reporting that the harness
# builds no engines at all, one commit after the split. Read the pair.
def module_text(cppm):
    text = open(cppm, encoding="utf-8").read()
    impl = cppm[:-len(".cppm")] + ".cpp"
    if os.path.exists(impl):
        text += "\n" + open(impl, encoding="utf-8").read()
    return text

m = json.load(open(sys.argv[1], encoding="utf-8"))
spec = module_text(sys.argv[2])
registry = module_text(sys.argv[3])
fail = []

# `scenario_from` is the harness's parser: what it accepts IS the axis.
accepted = set(re.findall(r'if \(s == "([a-z-]+)"\)\s*return Scenario::', spec))
for s in m["axes"]["scenario"]:
    if s not in accepted:
        fail.append(f"axes.scenario '{s}' is not accepted by bench::scenario_from "
                    f"(it accepts {sorted(accepted)})")

# Engines are whatever the registry constructs.
known = set(re.findall(r'make_(\w+)_engine', registry)) | set(
    re.findall(r'"(mcpp|cmake|xmake|bazel)"', registry))
for e in m["axes"]["engine"]:
    if e not in known:
        fail.append(f"axes.engine '{e}' is not built by bench/src/registry.cppm")

if fail:
    print("FAIL: bench/matrix.json disagrees with the harness")
    for f in fail:
        print("  " + f)
    raise SystemExit(1)
print("axes agree with the harness (scenarios via scenario_from, engines via the registry)")
PY

# ── 5: every number in the root README exists in the published data ────────
#
# The tables are generated (bench/tools/report.py) precisely so nobody types
# them, but a human still pastes the output — and a pasted number that drifts
# from the run it claims to come from is unfalsifiable by any other test. The
# numbers still print, they are just of something else, which is this suite's
# entire failure mode in miniature.
python3 - "$ROOT" <<'PYREADME'
import json, os, re, sys

root = sys.argv[1]

# ONE TABLE, THREE RUNS — each column named with the file it came from, because
# the table cannot be taken in a single run and pretending otherwise is how a
# number outlives the measurement it describes:
#   default mcpp + cmake  the five-arm run
#   xmake                 re-measured after the `-P`/cwd defect (cold 0.60s)
#   schedule=on           re-measured after the object-edge defect (§8b), where
#                         `touch-hub 0.22s` was timing a build still in flight
SOURCES = {
    "main":  "bench/results/pinned-workloads-20260813/mcpp-linux-gcc-5way.json",
    "xmake": "bench/results/pinned-workloads-20260813/mcpp-linux-gcc-xmake-refixed.json",
    "sched": "bench/results/schedule-refix-20260814/mcpp-linux-gcc-schedule-refixed.json",
}
truth = {}
for tag, rel in SOURCES.items():
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        print(f"FAIL: {rel} is missing — the root README quotes it")
        raise SystemExit(1)
    truth[tag] = {}
    for c in json.load(open(path, encoding="utf-8"))["cells"]:
        if c["status"] == "ok":
            truth[tag].setdefault(c["engine"], {})[c["scenario"]] = round(c["median_s"], 2)

def arm(tag, suffix=""):
    return next((k for k in truth[tag]
                 if k.startswith("mcpp@") and k.endswith(suffix)
                 and ("+" in k) == bool(suffix)), None)

default = arm("main")
sched   = arm("sched", "+schedule=on")
if not default or not sched:
    print(f"FAIL: could not find both mcpp arms (default={default}, schedule={sched})")
    raise SystemExit(1)

readme = open(os.path.join(root, "README.md"), encoding="utf-8").read()

# ⚠️ BOLD IS NOT PART OF THE GRAMMAR. This used to require `**Ns**` in the
# schedule column, which silently stopped matching the moment the bolding moved
# to whichever column is actually faster — two of the five rows dropped out and
# the check went on printing a success line for the three that remained.
# Emphasis is stripped first, and the row count is asserted against the table's
# own length, so a shape change fails loudly instead of narrowing the check.
table = re.search(r"^\| scenario \| what changed \|.*?(?=\n\n)", readme, re.S | re.M)
if not table:
    print("FAIL: the root README benchmark table did not parse — has its shape changed?")
    raise SystemExit(1)
plain = table.group(0).replace("**", "")
body  = [l for l in plain.splitlines() if re.match(r"^\| `[\w-]+` \|", l)]
rows  = re.findall(r"^\| `([\w-]+)` \| [^|]+ \| ([\d.]+)s · [\d.]+x \| ([\d.]+)s · [\d.]+x"
                   r" \| ([\d.]+)s · [\d.]+x \| ([\d.]+)s · [\d.]+x",
                   "\n".join(body), re.M)
if len(rows) != len(body):
    print(f"FAIL: parsed {len(rows)} of {len(body)} table rows — the check would "
          f"have covered only part of the table")
    raise SystemExit(1)

bad = []
for sc, s_sched, s_mcpp, s_cmake, s_xmake in rows:
    for tag, engine, claimed in (("sched", sched,   s_sched),
                                 ("main",  default, s_mcpp),
                                 ("main",  "cmake", s_cmake),
                                 ("xmake", "xmake", s_xmake)):
        have = truth[tag].get(engine, {}).get(sc)
        if have is None or abs(float(claimed) - have) >= 0.01:
            bad.append(f"README {sc}/{engine}={claimed}s but {SOURCES[tag]} says {have}")
if bad:
    print("FAIL: the root README quotes numbers that are not in the published run")
    for b in bad:
        print("  " + b)
    raise SystemExit(1)
print(f"root README: {len(rows)} rows x 4 engines all match their published runs")
PYREADME

# ── 4: the runner reads the file, and does not repeat it ───────────────────
#
# The matrix used to be run by .github/workflows/bench.yml. That workflow is
# gone — 12 of its 32 foreign-engine arms were waived, so a third of the
# comparison never ran while the job went green, and a shared runner measures
# the runner (243s there against 79s on a developer box for the same tree).
# bench/run-standard.sh took its place, and the invariant is unchanged: ONE
# list, read rather than repeated.
# The literal path, not `$MATRIX`: the variable is referenced all over the
# script, so matching it made this check pass even after the assignment was
# repointed at /dev/null — which is exactly what the negative test for it did.
grep -qE '^[^#]*MATRIX=.*bench/matrix\.json' "$RUNNER" \
  || { echo "FAIL: bench/run-standard.sh does not read bench/matrix.json — the matrix has been re-hardcoded"; exit 1; }

# The standard data set is THREE runs, and the runner must not quietly change it.
#
# n=1 has no dispersion at all, and every table this suite has published so far
# carried an `n=1` caveat asking readers not to compare digits — a caveat that
# is really an admission. Three samples with min/max is the least that makes a
# published number readable. `--runs 1` stays available for a quick check and is
# marked as not publishable in the script's own output.
#
# Checked structurally, not by counting lines: the first version of a check like
# this used `grep -A4` to reach a default and the comment above it pushed the
# default out of range, so it failed on a correct file.
python3 - "$RUNNER" <<'PYRUNS' || exit 1
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"^RUNS=(\d+)\s*$", text, re.M)
if not m:
    print("FAIL: bench/run-standard.sh no longer defines RUNS — this check cannot")
    print("      compare anything and must not pass silently")
    sys.exit(1)
if m.group(1) != "3":
    print(f"FAIL: the standard set takes 3 runs; bench/run-standard.sh defaults to {m.group(1)}")
    print("      n=1 has no dispersion, which is why every earlier table needed a")
    print("      caveat telling readers not to compare its digits.")
    sys.exit(1)
PYRUNS

# The runner must never hand the harness a BARE `mcpp`.
#
# A bare name resolves through PATH to the xlings shim, which RE-PICKS its
# version from the working directory — and for a `--project` run that directory
# is the measured tree, which carries its own pin. The first full local run
# measured mcpp@2026.8.11.3 in every single cell: the released binary, not the
# branch, and with no old-vs-new column at all. Nothing failed; the report simply
# described a different program.
grep -qE '^[^#]*mcpp=\$MCPP_BIN' "$RUNNER" \
  || { echo "FAIL: bench/run-standard.sh does not pass the mcpp under test as an"
       echo "      explicit binary. A bare \`mcpp\` is the xlings shim, and it"
       echo "      re-resolves its version from the measured tree."; exit 1; }

# The runner must SELECT from matrix.json, never enumerate cells itself.
#
# The workflow this replaced grew an inline platform list once and it had to be
# guarded; a shell script is at least as easy to hard-code into. The tell is a
# project or toolchain name written down here rather than read.
for token in mcpp-2026 xlings-2026 synth- ; do
    if grep -qE "^[^#]*${token}" "$RUNNER"; then
        echo "FAIL: bench/run-standard.sh mentions '${token}...' outside a comment —"
        echo "      the cell list belongs to matrix.json and must be read, not repeated"
        exit 1
    fi
done

# SPEC.md must point at the data rather than restate it. A cell list in prose is
# the second copy this whole test exists to prevent.
grep -q 'matrix.json' "$SPEC" \
  || { echo "FAIL: bench/SPEC.md does not reference matrix.json"; exit 1; }

# §6. An engine may not be scheduled against a project whose build description
#     for that engine declares nothing to build.
#
# `bazel build //...` over a package with no rules EXITS 0 having compiled
# nothing, in ~0.2s. That is not a failure anywhere in the stack — bazel
# succeeded, the runner timed it, the report printed it — so it reached the
# matrix as `bazel/clang/release/cold/mcpp-2026.8.11.3  0.43s`, beside mcpp's
# 12s and cmake's 94s, and nothing was red.
#
# Checked statically here (no bazel required) because the adapter's own
# `unbuildable_reason` guard only runs on a machine that HAS bazel, and the
# matrix is edited far more often than the adapter.
python3 - "$ROOT" <<'PY' || exit 1
import json, pathlib, re, sys
root = pathlib.Path(sys.argv[1])
m = json.loads((root / "bench/matrix.json").read_text(encoding="utf-8"))
bad = []
for c in m["cells"]:
    proj = c.get("project", "")
    bf = root / "bench/projects" / c.get("buildfiles", proj)
    for eng in c.get("engines", "").split(","):
        if eng != "bazel":
            continue
        f = bf / "BUILD.bazel"
        # No file at all means the description is EMITTED PER RUN by
        # bench.fixture.buildfiles (the generated fixture works this way and
        # does declare a cc_binary). Only a checked-in description can be
        # judged from here; asserting on the generated ones from a static test
        # just re-implements the emitter, wrongly — this check's first run
        # failed exactly that way.
        if not f.exists():
            continue
        body = re.sub(r"#.*", "", f.read_text(encoding="utf-8"))
        # ANY rule, not `cc_*` specifically. Every bazel rule instantiation
        # carries a `name =` attribute; `load()`, `package()` and
        # `exports_files()` do not. Matching `cc_binary|cc_library` was wrong:
        # the working xlings description declares an `alias`, which is a real
        # rule that `bazel query kind(rule, //...)` returns, so the guard would
        # have failed a cell that builds perfectly well. The phantom this
        # catches is ZERO rules, which is what `Found 0 targets` means.
        if not re.search(r"^\s*name\s*=", body, re.M):
            bad.append(f"{proj}: engines lists bazel, but {f.relative_to(root)} "
                       f"declares no cc_binary/cc_library outside comments")
if bad:
    print("FAIL: a cell schedules an engine that would build nothing:")
    for b in bad:
        print("  " + b)
    print("  such a cell reports `ok` with a ~0.2s number; drop the engine from the")
    print("  cell and record it under matrix.json `excluded`.")
    sys.exit(1)
print(f"no cell schedules bazel against a ruleless package ({len(m['cells'])} cells)")
PY

# §7. No engine adapter may branch on the LITERAL compiler request.
#
# main.cpp resolves `--compiler payload:clang` into an absolute driver path
# before any engine sees it, so `job.compiler == "clang"` is false in exactly
# the cells that mean clang. That rewrite has now broken three separate checks:
#   * `payload_toolchain` — --toolchain=mcpp-* was never passed at all
#   * `--toolchain=llvm`  — xmake fell back to g++ with clang's flags:
#                           `g++: unrecognized command-line option
#                           '--no-default-config'`, six fixture cells red
#   * (the same shape would hit any new one written the same way)
#
# Each time it looked correct in review, because the string being compared is
# the string the user typed. Adapters must key off the RESOLVED PATH instead.
python3 - "$ROOT" <<'PY' || exit 1
import pathlib, re, sys
root = pathlib.Path(sys.argv[1]) / "bench/src/engines"

# ⚠️ AN EMPTY GLOB PASSES THIS CHECK PERFECTLY. Rename the directory, move the
# adapters, and every assertion below iterates zero files and prints its success
# line. That is the failure mode this whole test exists to prevent, so the check
# must first prove it has something to check. Four is the number of adapters
# today (mcpp, cmake, xmake, bazel) plus engine; the floor is deliberately low
# so adding one does not require editing this.
#
# ⚠️ AND THE IMPLEMENTATION UNITS ARE WHERE THE CODE IS. The adapters declare in
# `.cppm` and define in `.cpp`; globbing only `.cppm` leaves this scanning
# signatures, where a `compiler == "clang"` cannot appear — so the check would
# have kept printing its success line while testing nothing at all. It nearly
# did: the split that moved the bodies did not touch this line.
adapters = sorted(root.glob("*.cppm")) + sorted(root.glob("*.cpp"))
if len(adapters) < 6:
    print(f"FAIL: only {len(adapters)} engine adapter files found under {root} — "
          f"this check cannot mean anything with so few, so something has moved")
    sys.exit(1)
if not any(f.suffix == ".cpp" for f in adapters):
    print(f"FAIL: no implementation units under {root} — the adapter bodies are "
          f"what this check reads, and it is looking at declarations only")
    sys.exit(1)

bad = []
for f in adapters:
    # engine's resolve_cxx() is the NORMALISER — comparing there is how a bare
    # `gcc` becomes `g++`, and it runs before any rewrite. Everything else sees
    # the resolved path.
    if f.stem == "engine":
        continue
    for n, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
        code = line.split("//", 1)[0]
        if re.search(r'compiler\s*==\s*"(clang|gcc)"', code):
            bad.append(f"{f.name}:{n}: {line.strip()[:90]}")
if bad:
    print("FAIL: an engine adapter compares job.compiler to a literal:")
    for b in bad:
        print("  " + b)
    print("  main.cpp rewrites payload:* into a path first, so that test never fires.")
    print("  Key off the resolved driver path (see payload_toolchain).")
    sys.exit(1)
print("no engine adapter branches on the literal compiler request")
PY

# §8. Every timing quoted in a bench/README table exists in the published data.
#
# §5 does this for the root README, which carries five rows. bench/README.md
# carries about ninety across seven tables — it is where nearly every number the
# project publishes actually lives, and it had no check at all.
#
# The failure this prevents has already happened once, and was caught by hand
# with one command to spare: the mcpp workload's xmake column was about to be
# published as `cold 0.60s` — a phantom from the run where xmake's `-P`/cwd
# disagreement meant every "cold" build measured an already-up-to-date tree. The
# real figure, 90.30s, lives in a different result file. Nothing about the README
# would have looked wrong; 0.60s is simply a number, and a fast one.
#
# Deliberately a WIDE net rather than a structured parse: any `NN.NNs` inside a
# table row must appear as some cell's median. It does not check that the number
# is in the RIGHT row — §5 does that for the table the most people see — but it
# does make an invented or stale figure impossible, and it needs no per-table
# schema, so it keeps working when a table is added.
#
# Table rows only, and two decimals with no space before the `s`: prose quotes
# measurements from other instruments in other formats ("makespan 79.79 s",
# "0.3 s"), and those are not cells of any bench run.
python3 - "$ROOT" <<'PY' || exit 1
import json, glob, os, pathlib, re, sys
root = pathlib.Path(sys.argv[1])

published = set()
files = sorted(glob.glob(str(root / "bench/results/**/*.json"), recursive=True))
for f in files:
    try:
        doc = json.load(open(f, encoding="utf-8"))
    except Exception:
        continue                       # hyperfine exports and other shapes
    # A bench report is an OBJECT. Engine scratch (compile_commands.json, bazel
    # exports) is often an ARRAY, and `.get` on one raised AttributeError right
    # out of this loop — a guard that crashes is a guard that stops guarding.
    if not isinstance(doc, dict):
        continue
    for c in doc.get("cells", []):
        if c.get("status") == "ok" and isinstance(c.get("median_s"), (int, float)):
            published.add(round(float(c["median_s"]), 2))

# An empty set would make every README trivially "clean" — the same silent pass
# the whole test exists to prevent, and one `git mv` of bench/results away.
if len(published) < 50:
    print(f"FAIL: only {len(published)} published medians found in "
          f"bench/results/ ({len(files)} files) — this check cannot mean "
          f"anything with so few, so something has moved")
    sys.exit(1)

bad = []
for name in ["bench/README.md", "bench/README.zh-CN.md"]:
    for n, line in enumerate((root / name).read_text(encoding="utf-8").splitlines(), 1):
        if not line.lstrip().startswith("|"):
            continue
        for m in re.finditer(r"(?<![\d.])(\d+\.\d\d)s(?![\d])", line):
            if round(float(m.group(1)), 2) not in published:
                bad.append(f"{name}:{n}: {m.group(0)} is in no published run")
if bad:
    print("FAIL: a bench README table quotes a timing that was never measured:")
    for b in bad[:20]:
        print("  " + b)
    if len(bad) > 20:
        print(f"  ... and {len(bad) - 20} more")
    print("  Either the number is invented, or it comes from a run that was not")
    print("  committed under bench/results/. A benchmark whose numbers cannot be")
    print("  traced to a run is a claim, not a measurement.")
    sys.exit(1)
print(f"bench READMEs: every quoted timing traces to bench/results/ "
      f"({len(published)} medians across {len(files)} files)")
PY

echo "bench matrix OK"
