#!/usr/bin/env bash
# requires: gcc python3
# 1065 -- `mcpp test` states a program's parts, each case as it happens, and the metrics, bounds,
# kind and tags of its cases, in its records and in the run it keeps (mcpp#917; docs/50 §8; mtest
# spec P5.1, P6, P10.2, P12).
#
# Criteria:
#   A. `--cases --message-format json`: a program of several files with cases in two of them has
#      `parts`, each `{name, file}` named as the run names it (`unit/a`, `unit/b`).
#   B. `--message-format json`: a `case` record for each case's beginning and end comes before
#      its program's record; the end carries the case's status, metrics and bounds; a part's record
#      states `part_of`; a case in the record's `cases` has its kind, metrics and bounds.
#   C. The run kept under target/mtest has, per case, `kind`, `started_ms`, `metrics` and `bounds`
#      (P12).
#   D. `self env` names the features `test-parts`, `test-case-stream` and `test-metrics`.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -40 "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null 2>&1
cd app
mkdir -p tests/unit
cat > tests/unit/a.cpp <<'CPP'
import mcpplibs.mtest;
import mcpplibs.mtest.bench;
using namespace mcpplibs::mtest;
Bench timed{"timed", at_most{"spin", 10.0}, [] {
    bench::measure("spin", [] { int x = 0; for (int i = 0; i < 100; ++i) bench::keep(x += i); },
                   {.samples = 3, .warmup = 0});
}};
CPP
cat > tests/unit/b.cpp <<'CPP'
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest plain{"plain", tag{"quick"}, [] { expect(eq(1 + 1, 2)); }};
CPP
printf '\n[test]\nprograms = "directory"\n' >> mcpp.toml

# A
"$MCPP" test --cases --message-format json > a.json 2> a.err || fail "A: --cases" a.json a.err
python3 - a.json <<'PY' || fail "A: the parts" a.json
import json, sys
recs = [json.loads(l) for l in open(sys.argv[1]) if l.startswith("{")]
unit = next(r for r in recs if r.get("test") == "unit")
names = sorted(p["name"] for p in unit["parts"])
assert names == ["unit/a", "unit/b"], names
assert all(p["file"].endswith(("a.cpp", "b.cpp")) for p in unit["parts"]), unit["parts"]
PY
echo "  ok  the listing names the parts a run divides the program into"

# B
"$MCPP" test --message-format json > b.json 2> b.err || fail "B: the run" b.json b.err
python3 - b.json <<'PY' || fail "B: the records" b.json
import json, sys
recs = [json.loads(l) for l in open(sys.argv[1]) if l.startswith("{")]
order = [("case", r["case"]["test"], r["case"]["event"], r["case"]["id"]) if "case" in r
         else ("test", r.get("test")) for r in recs if "case" in r or "test" in r]
for part in ("unit/a", "unit/b"):
    at = order.index(("test", part))
    events = [o for o in order[:at] if o[0] == "case" and o[1] == part]
    assert any(e[2] == "begin" for e in events) and any(e[2] == "end" for e in events), (part, order)
end = next(r["case"] for r in recs if "case" in r and r["case"]["event"] == "end" and r["case"]["id"] == "timed")
assert end["status"] == "pass", end
assert end["metrics"][0]["name"] == "spin" and end["metrics"][0]["unit"] == "s", end
assert end["bounds"] == [{"metric": "spin", "max": 10.0}], end
a = next(r for r in recs if r.get("test") == "unit/a")
assert a["part_of"] == "unit", a
case = next(c for c in a["cases"] if c["id"] == "timed")
assert case["kind"] == "bench" and case["metrics"] and case["bounds"], case
b = next(r for r in recs if r.get("test") == "unit/b")
plain = next(c for c in b["cases"] if c["id"] == "plain")
assert plain.get("tags") == ["quick"], plain
PY
echo "  ok  cases are records as they happen; parts, metrics, bounds, kind and tags are stated"

# C
python3 - target/mtest <<'PY' || fail "C: the store" target/mtest/index.ndjson
import json, os, sys
store = sys.argv[1]
run = [json.loads(l) for l in open(os.path.join(store, "index.ndjson"))][-1]
cases = [json.loads(l) for l in open(os.path.join(store, "runs", run["id"], "cases.ndjson"))]
timed = next(c for c in cases if c["id"] == "timed")
assert timed["kind"] == "bench" and timed["started_ms"] >= 0, timed
assert timed["metrics"][0]["name"] == "spin" and timed["bounds"][0]["metric"] == "spin", timed
PY
echo "  ok  the kept run holds kind, start, metrics and bounds per case"

# D
"$MCPP" self env --format json > d.json 2>&1 || fail "D: self env" d.json
for f in test-parts test-case-stream test-metrics; do
    grep -q "\"$f\"" d.json || fail "D: no feature $f" d.json
done
echo "  ok  the features are announced"
echo "PASS: 1065 test records carry parts, live cases and metrics"
