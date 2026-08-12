#!/usr/bin/env bash
# requires: python3
# bench/ harness: builds with mcpp, measures a fixture, and emits a valid report.
#
# This is an INTEGRATION test for the benchmark suite, not a benchmark: it uses
# the smallest fixture that still exercises the module graph, and asserts on the
# protocol rather than on any timing. Timings on CI are noise; the contract is not.
set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$REPO/bench"
"$MCPP" build > /dev/null

BENCH=$(find target -type f \( -name bench -o -name bench.exe \) | head -1)
[ -n "$BENCH" ] || { echo "harness binary not found under bench/target"; exit 1; }
BENCH="$REPO/bench/$BENCH"

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

# 2. A real measurement over the modules variant. Tiny on purpose: 4 units still
#    produce a module graph with depth, which is what the harness is for.
# On failure the child's build log is the only thing that explains why — and the
# trap deletes $TMP on exit, so a message that merely names the path is useless
# in CI. Dump it here instead of leaving a dangling reference.
dump_child_logs() {
    echo "--- harness stdout ---"; cat "$TMP/stdout.txt" 2>/dev/null
    for log in "$TMP"/work/*/bench-child.log; do
        [ -f "$log" ] || continue
        echo "--- $log ---"; tail -40 "$log"
    done
}

"$BENCH" --engines "mcpp=$MCPP" --variants modules --scenarios cold,noop \
         --units 4 --fanin 2 --weight 2 --runs 1 \
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

echo "bench harness OK"
