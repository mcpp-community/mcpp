#!/usr/bin/env bash
# requires: python3
# 1024 -- `mcpp run --print-launch` takes the fast path (design 2026-11-07
# §4.9, D19). The full path records the launch it composes in the
# configuration's directory; with nothing changed, the fast path describes the
# launch from that record when the launch it composes itself is the recorded one.
#
# Criteria:
#   A. Unchanged: the second description is the first, with this request's
#      program arguments, and is made without resolving (no `Resolving` line).
#   B. A changed source is compiled and the launch is described the same way.
#   C. A record that is not the fast path's own composition is not used: the full
#      path describes the launch and records it again.
#   D. `--message-format json`: every line of standard output is a JSON document,
#      a rebuild reports `progress`, and the last line is the `mcpp.launch`
#      envelope. Without `--print-launch` it is refused, exit 2.
#   E. `--protocol-version` states `launch-stream`.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
"$MCPP" build > build.log 2>&1 || fail "the project does not build" build.log

# A
"$MCPP" run --print-launch > full.json 2> full.err || fail "A: the first description failed" full.err
"$MCPP" run --print-launch -- one two > fast.json 2> fast.err || fail "A: the second description failed" fast.err
grep -q 'Resolving' fast.err && fail "A: the unchanged description resolved again (the full path)" fast.err
$PY - full.json fast.json <<'PY' || fail "A: the fast description is not the full path's" full.json fast.json
import json, sys
full, fast = (json.load(open(p)) for p in sys.argv[1:3])
assert fast["kind"] == "mcpp.launch", fast
f, g = full["data"], fast["data"]
assert g["argv"][-2:] == ["one", "two"], g["argv"]
g = dict(g, argv=g["argv"][:-2])
assert f == g, (f, g)
PY

# B
src=$(ls src/*.cpp | head -1)
printf '\n// changed\n' >> "$src"
"$MCPP" run --print-launch > changed.json 2> changed.err || fail "B: the changed description failed" changed.err
grep -q 'Compiling' changed.err || fail "B: the changed source was not compiled" changed.err
$PY - full.json changed.json <<'PY' || fail "B: the description after a change differs" full.json changed.json
import json, sys
full, changed = (json.load(open(p))["data"] for p in sys.argv[1:3])
assert full == changed, (full, changed)
PY

# C
record=$(find target -path '*/launch/*.json' | head -1)
[[ -n "$record" ]] || fail "C: no launch is recorded in the configuration's directory"
$PY - "$record" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); r["env"]["MCPP_E2E_FOREIGN"] = "1"
json.dump(r, open(sys.argv[1], "w"))
PY
"$MCPP" run --print-launch > again.json 2> again.err || fail "C: the description failed" again.err
grep -q 'Resolving' again.err || fail "C: a foreign record was used by the fast path" again.err
grep -q MCPP_E2E_FOREIGN again.json && fail "C: the foreign field reached the description" again.json
grep -q MCPP_E2E_FOREIGN "$record" && fail "C: the full path did not record the launch again" "$record"

# D
printf '\n// changed again\n' >> "$src"
"$MCPP" run --print-launch --message-format json > stream.ndjson 2> stream.err || fail "D: the streamed description failed" stream.err
$PY - stream.ndjson <<'PY' || fail "D: the stream is not NDJSON ending in the envelope" stream.ndjson
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
assert any(l.get("event") == "progress" for l in lines[:-1]), lines
assert lines[-1]["kind"] == "mcpp.launch" and lines[-1]["data"]["argv"], lines[-1]
PY
set +e
"$MCPP" run --message-format json > refused.out 2> refused.err
rc=$?
set -e
[[ $rc -eq 2 ]] || fail "D: --message-format json without --print-launch exited $rc, not 2" refused.err
grep -q 'print-launch' refused.err || fail "D: the refusal does not name --print-launch" refused.err

# E
"$MCPP" --protocol-version > e.out 2>&1 || fail "E: --protocol-version failed" e.out
grep -q '"launch-stream"' e.out || fail "E: launch-stream is not stated" e.out

echo "PASS: 1024 a launch description takes the fast path"
