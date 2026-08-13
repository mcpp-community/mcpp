#!/usr/bin/env bash
# requires: python3
# bench/ harness: builds with mcpp, measures a fixture, and emits a valid report.
#
# This is an INTEGRATION test for the benchmark suite, not a benchmark: it uses
# the smallest fixture that still exercises the module graph, and asserts on the
# protocol rather than on any timing. Timings on CI are noise; the contract is not.
set -e

# bench/tests -> two levels up is the repository root.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$REPO/bench"
"$MCPP" build > /dev/null

# NEWEST, not `find | head -1`: target/ holds one directory per toolchain
# fingerprint, so a tree built more than once has several binaries with this
# name and `head -1` picks whichever the filesystem lists first — routinely a
# stale one. That is a test exercising code that has already been replaced, with
# no symptom at all. See .github/tools/newest_artifact.sh.
BENCH="$REPO/bench/$(bash "$REPO/.github/tools/newest_artifact.sh" target bench)"

# 1. Availability listing must classify mcpp itself as present. If this fails the
#    probe path is broken, and every later cell would be reported `unavailable`
#    for the wrong reason.
# Engines are named by BINARY, not by PATH lookup: `$MCPP` is the build under
# test, while a bare `mcpp` resolves to whatever the sandbox has — on CI that is
# an xlings shim reporting "'mcpp' is not installed", which failed every cell.
out=$("$BENCH" --list --engines "mcpp=$MCPP")
# The label carries the version it discovered ("mcpp@2026.8.12.1"), which is what
# makes a two-binary comparison legible; match the prefix, not the whole token.
echo "$out" | grep -qE '^mcpp(@[^ ]+)? +yes' || { echo "mcpp not reported available:"; echo "$out"; exit 1; }
# The note carries each engine's reported VERSION, so a result file can answer
# "which cmake produced this?". Some tools colour that banner, and the escape
# sequences must be stripped before they reach a JSON result — an ESC here means
# the CSI parser regressed (it once left the "0m" of every colour reset behind).
# Written as an explicit `if` rather than `grep -q ... && { ... }`: under
# `set -e` the exit status of an AND-OR list whose left side fails is the exact
# corner this suite has been bitten by before.
if printf '%s' "$out" | grep -q "$(printf '\033')"; then
    echo "engine notes contain ANSI escapes:"; printf '%s' "$out" | cat -v; exit 1
fi

# 2. A real measurement over the modules variant. Tiny on purpose: 4 units still
#    produce a module graph with depth, which is what the harness is for.
# On failure the child's build log is the only thing that explains why — and the
# trap deletes $TMP on exit, so a message that merely names the path is useless
# in CI. Dump it here instead of leaving a dangling reference.
dump_child_logs() {
    echo "--- harness stdout ---"; cat "$TMP/stdout.txt" 2>/dev/null
    for log in "$TMP"/work/logs/*.log; do
        [ -f "$log" ] || continue
        echo "--- $log ---"; tail -40 "$log"
    done
}

# --preset names the size instead of spelling it out, which is also the only
# place the preset code path gets exercised.
"$BENCH" --engines "mcpp=$MCPP" --variants modules --scenarios cold,noop \
         --preset smoke --runs 1 \
         --work "$TMP/work" --out "$TMP/report.json" > "$TMP/stdout.txt" \
  || { echo "harness exited non-zero"; dump_child_logs; exit 1; }

# 3. The report must be a protocol-shaped document, not merely non-empty.
grep -q '"protocol_version": 1' "$TMP/report.json" \
  || { echo "report is missing protocol_version"; cat "$TMP/report.json"; exit 1; }
grep -q '"status": "ok"' "$TMP/report.json" \
  || { echo "no cell succeeded"; cat "$TMP/report.json"; dump_child_logs; exit 1; }

# 4. INVARIANT 1: a non-ok cell must never carry a timing. Asserted from BOTH
#    sides — checking only that ok cells have medians would pass a harness that
#    emitted medians for everything, which is exactly the bug this protocol was
#    designed to make impossible.
python3 - "$TMP/report.json" <<'PY'
import json, sys
cells = json.load(open(sys.argv[1]))["cells"]
assert cells, "report has no cells"
for c in cells:
    if c["status"] == "ok":
        assert "median_s" in c, f"ok cell without a median: {c}"
        assert c["runs"] > 0, f"ok cell with zero runs: {c}"
    else:
        assert "median_s" not in c, f"non-ok cell carrying a timing: {c}"
        assert c["note"], f"non-ok cell without a reason: {c}"
PY

# 5. Host facts must be populated — a result without its host is not comparable
#    to anything, so an empty one is a defect rather than a cosmetic gap.
python3 - "$TMP/report.json" <<'PY'
import json, sys
h = json.load(open(sys.argv[1]))["host"]
assert h["os"], "host.os is empty"
assert h["logical_cores"] >= 1, f"implausible core count: {h}"
assert h["arch"] != "unknown", f"arch not detected: {h}"
PY

# 6. The three fixture variants must all generate and differ in SHAPE, not just
#    in file names: modules-impl is the variant whose whole point is that bodies
#    live outside the interface unit.
"$BENCH" --engines "mcpp=$MCPP" --variants headers,modules,modules-impl --scenarios noop \
         --units 3 --fanin 1 --weight 1 --runs 1 \
         --work "$TMP/w2" --out "$TMP/r2.json" > /dev/null
# Directory names are slugged from the engine label, which carries a version, so
# resolve them by suffix instead of hard-coding the label.
hdr=$(echo "$TMP"/w2/*-headers);      mods=$(echo "$TMP"/w2/*-modules)
impl=$(echo "$TMP"/w2/*-modules-impl)
[ -f "$hdr/include/unit_0.hpp" ]   || { echo "headers variant missing its header"; exit 1; }
[ -f "$mods/src/unit_0.cppm" ]     || { echo "modules variant missing its interface"; exit 1; }
[ -f "$impl/src/unit_0_impl.cpp" ] || { echo "modules-impl variant has no implementation unit"; exit 1; }
grep -q 'export int unit_0_value();' "$impl/src/unit_0.cppm" \
  || { echo "modules-impl interface should DECLARE, not define"; exit 1; }
grep -q 'export int unit_0_value() {' "$mods/src/unit_0.cppm" \
  || { echo "modules interface should DEFINE inline"; exit 1; }

# 7. No fixture may say `import std;`. Engines differ wildly in std-module
#    support and that difference would dominate every measurement — the suite
#    measures module machinery, not std-module support.
if grep -rq 'import std;' "$TMP/w2"/*/src/ 2>/dev/null; then
    echo "a generated fixture imports std, which breaks cross-engine comparability"
    exit 1
fi

# 8. A RELATIVE engine program path must still resolve. Every measured command
#    runs with its cwd set to the project under test, so `--engines mcpp=./bin`
#    used to resolve against the fixture and fail to spawn — reported per cell as
#    `exited -1` across the whole matrix, with an empty log to explain it.
#
#    The run happens from the binary's OWN directory, with the fixture under
#    $TMP: that is all the bug needs (cwd at launch != the tree the child is
#    later run in) and it is expressible everywhere. Deriving a relative path
#    between two arbitrary directories is not — on Windows `$MCPP` and `$TMP`
#    routinely sit on different drives (`path is on mount 'D:', start on mount
#    'C:'`), and on macOS `mktemp -d` returns `/var/folders/...` while the
#    process's real cwd is `/private/var/folders/...`, one level deeper.
#
#    The binary is REFERENCED where it is, never copied: mcpp locates its
#    payloads relative to its own installation, so a copy in a scratch dir would
#    fail for a reason that has nothing to do with the path handling under test.
BINDIR=$(dirname "$MCPP")
BINNAME=$(basename "$MCPP")
( cd "$BINDIR" \
  && "$BENCH" --engines "mcpp=./$BINNAME" --variants modules --scenarios cold \
              --units 3 --fanin 1 --weight 1 --runs 1 \
              --work "$TMP/w3" --out "$TMP/r3.json" > "$TMP/stdout3.txt" ) \
  || { echo "harness exited non-zero on a relative engine path"; cat "$TMP/stdout3.txt"; exit 1; }
python3 - "$TMP/r3.json" <<'PY'
import json, sys
cells = json.load(open(sys.argv[1]))["cells"]
assert cells, "no cells for a relative engine path"
bad = [c for c in cells if c["status"] != "ok"]
assert not bad, f"relative engine path did not resolve: {bad}"
PY

# 9. And a program that cannot be run at all must be reported with a reason that
#    stands on its own. Here the probe catches it first (`unavailable`), but the
#    invariant is the same one `failure_note` enforces further in: never point a
#    reader at a log the child never got far enough to write.
"$BENCH" --engines "mcpp=$TMP/definitely-not-here" --variants modules --scenarios cold \
         --units 3 --fanin 1 --weight 1 --runs 1 \
         --work "$TMP/w4" --out "$TMP/r4.json" > /dev/null 2>&1 || true
python3 - "$TMP/r4.json" <<'PY'
import json, sys
cells = json.load(open(sys.argv[1]))["cells"]
assert cells, "no cells for a missing engine binary"
for c in cells:
    assert c["status"] != "ok", f"a missing binary produced a timing: {c}"
    assert c["note"], f"a missing binary produced no reason: {c}"
    assert "see " not in c["note"], \
        f"reason points at a log that was never written: {c['note']}"
PY

# 10. --hub/--leaf/--body are PROJECT-RELATIVE, and must resolve from anywhere.
#
#     They used to be taken as given, i.e. relative to the harness's own working
#     directory. That is the project directory only when you are benchmarking
#     the tree you are standing in — true for mcpp measuring itself, false for
#     every other project — and it fails SILENTLY: exists() says no, the cell
#     reports `skipped --hub points at a file that does not exist`, and the run
#     still exits 0. Three CI jobs reported success with zero measurements.
#
#     Driven from a different cwd on purpose; that difference IS the bug.
mkdir -p "$TMP/proj/src"
cat > "$TMP/proj/mcpp.toml" <<'TOML'
[package]
name    = "relhub"
version = "0.1.0"
TOML
printf 'export module hub;\nexport int hub_value() { return 1; }\n' > "$TMP/proj/src/hub.cppm"
printf 'import hub;\nint main() { return hub_value() - 1; }\n'      > "$TMP/proj/src/main.cpp"

( cd "$TMP" \
  && "$BENCH" --engines "mcpp=$MCPP" --project "$TMP/proj" --variants native \
              --scenarios touch-hub,edit-body --runs 1 \
              --hub src/hub.cppm --body src/hub.cppm \
              --work "$TMP/w5" --out "$TMP/r5.json" > "$TMP/stdout5.txt" 2>&1 ) \
  || { echo "harness exited non-zero on project-relative targets"; cat "$TMP/stdout5.txt"; exit 1; }
python3 - "$TMP/r5.json" <<'PY'
import json, sys
cells = json.load(open(sys.argv[1]))["cells"]
assert cells, "no cells for project-relative targets"
missing = [c["note"] for c in cells if "does not exist" in c["note"]]
assert not missing, ("--hub/--body were resolved against the harness's cwd "
                     f"rather than the project: {missing}")
PY

# 11. EXIT STATUS. A run that measured nothing must not report success — the
#     whole matrix did exactly that for weeks (6 ok / 48 failed / 18
#     unavailable, and green), as did an xlings job whose every cell was
#     skipped. Asserted from BOTH sides, because a harness that always exited
#     non-zero would sail through a one-sided check: every successful run above
#     is the other half.
if "$BENCH" --engines "mcpp=$TMP/definitely-not-here" --variants modules \
            --scenarios cold --units 3 --fanin 1 --weight 1 --runs 1 \
            --work "$TMP/w6" --out "$TMP/r6.json" > /dev/null 2>&1; then
    echo "a run in which nothing was measured exited 0"; exit 1
fi

# 12. --timeout must KILL a child rather than wait on it, and must say that is
#     what happened. Without it a hung engine consumes the whole CI budget and
#     the log stays empty, because a cell only prints once it is over: two jobs
#     sat 25 minutes inside one child that way. One second is far below any real
#     cold build, so the deadline is certain to fire.
"$BENCH" --engines "mcpp=$MCPP" --variants modules --scenarios cold \
         --units 3 --fanin 1 --weight 1 --runs 1 --timeout 1 \
         --work "$TMP/w7" --out "$TMP/r7.json" > "$TMP/stdout7.txt" 2>&1 || true
grep -qi 'timed out' "$TMP/stdout7.txt" || {
    echo "a 1-second deadline neither fired nor was reported:"
    cat "$TMP/stdout7.txt"; exit 1; }

echo "bench harness OK"
