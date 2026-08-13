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
# ROOT is passed in: this python runs from stdin, so sys.argv[0] is "-" and the
# repository cannot be derived from it.
python3 - "$MATRIX" "$ROOT" <<'PY'
import json, os, re, sys

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
        # mcpp is this checkout; anything else is a submodule under bench/projects/.
        tree = root if c["project"] == "mcpp" else os.path.join(
            root, "bench", "projects", c.get("buildfiles", c["project"]), c["project"])
        if not os.path.isdir(tree):
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: no tree at {tree} "
                        "(run `git submodule update --init`)")
            break
        target = os.path.join(tree, c[field])
        if not os.path.isfile(target):
            fail.append(f"{c['os']}/{c['toolchain']}/{c['project']}: {field}='{c[field]}' does not "
                        f"exist in the pinned tree — every scenario that perturbs it would be "
                        f"reported `skipped` and the job would still pass")

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
    ws = json.load(open(xlings_pin)).get("workspace", {}).get("mcpp")
    if ws and ws != m.get("reference_mcpp"):
        fail.append(f"reference_mcpp={m.get('reference_mcpp')} but .xlings.json bootstraps "
                    f"mcpp {ws} — the reference arm IS the bootstrapped binary, so these "
                    f"two must agree or the old-vs-new column compares the wrong release")

if fail:
    print("FAIL: bench/matrix.json")
    for f in fail:
        print("  " + f)
    raise SystemExit(1)
print(f"matrix: {len(m['cells'])} cells, {len(m.get('excluded', []))} documented exclusions, "
      f"baseline={base}, tool pins {m['tools']['cmake']}/{m['tools']['xmake']}/{m['tools']['bazel']}")
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
