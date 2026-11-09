#!/usr/bin/env bash
# requires: gcc python3
# 1050 -- a test run is kept in the mtest store's form (docs/08; mtest spec P12).
#
# `mcpp test` writes `target/mtest/runs/<id>/` -- `run.json`, a line per process in
# `programs.ndjson`, a line per case in `cases.ndjson` -- and appends the run to
# `target/mtest/index.ndjson`. `[test] store` names another directory; `"none"` keeps nothing.
#
# Criteria: a run is one record of the index with its counts; each process states its status,
# duration, start and worker; each case its program and status; a second run is a second record;
# `store = "none"` adds none; `store = "<dir>"` writes there.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python

cd "$TMP"
"$MCPP" new prog > new.log 2>&1 || fail "mcpp new" new.log
cd prog
"$MCPP" test > ../t1.log 2>&1 || fail "the first run failed" ../t1.log
[ -f target/mtest/index.ndjson ] || fail "no index" ../t1.log
"$PY" - <<'EOF' || fail "the stored run is not as stated" target/mtest/index.ndjson
import json, pathlib
store = pathlib.Path("target/mtest")
runs = [json.loads(l) for l in (store / "index.ndjson").read_text().splitlines() if l.strip()]
assert len(runs) == 1, runs
run = runs[0]
assert run["schema"] == "mtest.run/1" and run["status"] == "pass", run
assert run["counts"].get("pass", 0) >= 1, run
d = store / "runs" / run["id"]
assert json.loads((d / "run.json").read_text())["id"] == run["id"]
programs = [json.loads(l) for l in (d / "programs.ndjson").read_text().splitlines() if l.strip()]
assert programs and all(p["status"] == "pass" and p["ms"] >= 0 and p["started_ms"] >= 0
                        and p["worker"] >= 0 for p in programs), programs
cases = [json.loads(l) for l in (d / "cases.ndjson").read_text().splitlines() if l.strip()]
assert cases and all(c["status"] == "pass" and c["program"] for c in cases), cases
EOF
echo "  ok  a run is kept with its processes and cases"

"$MCPP" test > ../t2.log 2>&1 || fail "the second run failed" ../t2.log
[ "$(wc -l < target/mtest/index.ndjson | tr -d ' ')" = 2 ] || fail "a second run is not a second record" target/mtest/index.ndjson
printf '\n[test]\nstore = "none"\n' >> mcpp.toml
"$MCPP" test > ../t3.log 2>&1 || fail "the third run failed" ../t3.log
[ "$(wc -l < target/mtest/index.ndjson | tr -d ' ')" = 2 ] || fail "store = none kept a run" target/mtest/index.ndjson
sed -i.bak 's|^store = "none"$|store = "out/test-runs"|' mcpp.toml
"$MCPP" test > ../t4.log 2>&1 || fail "the fourth run failed" ../t4.log
[ -f out/test-runs/index.ndjson ] || fail "store = <dir> did not write there" ../t4.log
echo "  ok  each run is a record; store = none keeps nothing; store = <dir> writes there"
echo "PASS: 1050 a test run is kept in the mtest store"
