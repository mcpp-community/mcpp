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

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MATRIX="$ROOT/bench/matrix.json"
WORKFLOW="$ROOT/.github/workflows/bench.yml"
SPEC="$ROOT/bench/SPEC.md"

[ -f "$MATRIX" ]   || { echo "FAIL: bench/matrix.json is missing"; exit 1; }
[ -f "$SPEC" ]     || { echo "FAIL: bench/SPEC.md is missing"; exit 1; }
[ -f "$WORKFLOW" ] || { echo "FAIL: .github/workflows/bench.yml is missing"; exit 1; }

# ── 1..3: the data ─────────────────────────────────────────────────────────
python3 - "$MATRIX" <<'PY'
import json, sys

m = json.load(open(sys.argv[1]))
axes = m["axes"]
fail = []

def check_list(where, field, value, axis):
    for v in value.split(","):
        v = v.strip()
        # `native` is the real-project variant: a tree has exactly one form,
        # its own, so it is not a generated axis value.
        if axis == "variant" and v == "native":
            continue
        if v not in axes[axis]:
            fail.append(f"{where}: {field}='{v}' is not in axes.{axis} {axes[axis]}")

seen = set()
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
    if c["os"] not in m["runners"]:
        fail.append(f"{where}: no runner declared for os='{c['os']}'")

# The baseline must be an engine, and it must actually be IN every cell it is
# supposed to normalise — a ratio against an engine that never ran is not a
# ratio, and the report renders it as bare seconds.
base = m["baseline"]
if base not in axes["engine"]:
    fail.append(f"baseline '{base}' is not one of axes.engine")
for c in m["cells"]:
    if base not in [e.strip() for e in c["engines"].split(",")]:
        fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: baseline '{base}' "
                    f"is not among its engines — that cell would report bare seconds")

# 3. Every excluded cell says why, and says something.
for x in m.get("excluded", []):
    if len(x.get("reason", "").strip()) < 20:
        fail.append(f"excluded {x.get('os')}/{x.get('toolchain')}/{x.get('project','*')}: "
                    "reason is missing or too short to be one")

# An exclusion must not also be a cell. `*` is a wildcard, and an exclusion that
# names an `engine` scopes a CAVEAT to one column rather than removing the job —
# those legitimately coexist with the cell.
def matches(x, c, key):
    v = x.get(key)
    return v is None or v == "*" or v == c[key]

for x in m.get("excluded", []):
    if x.get("engine"):
        continue
    for c in m["cells"]:
        if all(matches(x, c, k) for k in ("os", "toolchain", "project")):
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']} is both a cell and excluded")

if fail:
    print("FAIL: bench/matrix.json")
    for f in fail:
        print("  " + f)
    raise SystemExit(1)
print(f"matrix: {len(m['cells'])} cells, {len(m.get('excluded', []))} documented exclusions, "
      f"baseline={base}")
PY

# ── 2: the axis values are ones the harness accepts ────────────────────────
# Read out of the harness's own source, not a second list here — the whole
# point of this test is that there is no second list.
python3 - "$MATRIX" "$ROOT/bench/src/spec.cppm" "$ROOT/bench/src/registry.cppm" <<'PY'
import json, re, sys

m = json.load(open(sys.argv[1]))
spec = open(sys.argv[2], encoding="utf-8").read()
registry = open(sys.argv[3], encoding="utf-8").read()
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

# ── 4: the workflow reads the file, and does not repeat it ─────────────────
grep -q 'bench/matrix.json' "$WORKFLOW" \
  || { echo "FAIL: bench.yml does not read bench/matrix.json — the matrix has been re-hardcoded"; exit 1; }

# The old shape enumerated runner images inline. If that ever comes back, the
# two copies disagree the first time a runner image is bumped in one of them.
if grep -qE '^\s*case ",\$want," in \*,(linux|macos|windows),\*\)' "$WORKFLOW"; then
    echo "FAIL: bench.yml still enumerates platforms inline; matrix.json owns that list"
    exit 1
fi
for img in $(python3 -c "import json,sys;print(' '.join(json.load(open(sys.argv[1]))['runners'].values()))" "$MATRIX"); do
    if grep -q "runs-on: $img" "$WORKFLOW"; then
        echo "FAIL: bench.yml hard-codes runner image '$img'; it must come from matrix.json"
        exit 1
    fi
done

# SPEC.md must point at the data rather than restate it. A cell list in prose is
# the second copy this whole test exists to prevent.
grep -q 'matrix.json' "$SPEC" \
  || { echo "FAIL: bench/SPEC.md does not reference matrix.json"; exit 1; }

echo "bench matrix OK"
