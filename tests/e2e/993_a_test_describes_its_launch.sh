#!/usr/bin/env bash
# requires: python3
# 993 -- `mcpp test <name> --print-launch` (docs/50 §9, feature `test-launch`;
# design 2026-10-30 part 2 §7.4): one test, built and described as `mcpp test`
# would start it, for a debugger to start.
#
# Criteria:
#   A. The `mcpp.launch` envelope names the test (`data.test`), its program
#      (`argv[0]` is `program`, which exists), `env` and `cwd`, and ends its argv
#      with the arguments after `--`; the test is not started.
#   B. What the envelope describes runs: the argv with the environment exits 0
#      and the test's own output appears.
#   C. A pattern several tests contain is resolved to the test named exactly.
#   D. Refused, exit 2: no pattern; a pattern naming several tests;
#      `--message-format json`; `--no-run`.
#   E. `--protocol-version` states `test-launch`.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
mkdir -p tests/unit
cat > tests/unit/math.cpp <<'EOF'
import std;
int main(int argc, char** argv) {
    std::ofstream("started-math") << argc;
    std::println("math ran with {} argument(s), last [{}]", argc - 1, argc > 1 ? argv[argc - 1] : "");
    return 0;
}
EOF
cat > tests/unit/math2.cpp <<'EOF'
import std;
int main() { std::ofstream("started-math2") << 1; return 0; }
EOF

# A
"$MCPP" test unit/math --print-launch -- --flag x > a.out 2> a.err || fail "A: --print-launch failed" a.err
[ ! -e started-math ] && [ ! -e target/started-math ] || fail "A: the test was started"
$PY - a.out <<'EOF' || fail "A: not the test's mcpp.launch envelope" a.out
import json, sys, os
same = lambda a, b: os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))
d = json.load(open(sys.argv[1]))
assert d["kind"] == "mcpp.launch", d["kind"]
data = d["data"]
assert data["test"] == "unit/math", data
assert os.path.isfile(data["program"]), data["program"]
argv = data["argv"]
assert any(same(a, data["program"]) for a in argv), data
assert argv[-2:] == ["--flag", "x"], argv
assert "env" in data and "cwd" in data
last = json.load(open("target/.last-result.json"))["data"]
record = json.load(open(last["result"]))["data"]["configuration"]
assert all(data["configuration"][key] == record[key] for key in ("triple", "profile", "platform")), data
assert isinstance(data["configuration"]["runners"], list), data
EOF
echo "ok: the envelope describes the test"

# B
$PY - a.out > b.out 2> b.err <<'EOF' || fail "B: what the envelope describes did not run" b.out b.err
import json, sys, os, subprocess
d = json.load(open(sys.argv[1]))["data"]
env = dict(os.environ)
env.update(d.get("env") or {})
r = subprocess.run(d["argv"], cwd=d["cwd"], env=env, capture_output=True, text=True)
sys.stdout.write(r.stdout)
sys.exit(r.returncode)
EOF
grep -q "math ran with 2 argument(s), last \[x\]" b.out || fail "B: the test's output" b.out
echo "ok: the description runs"

# C -- `unit/math` is contained in `unit/math2` too
grep -q '"unit/math"' a.out || fail "C: the exact name was not chosen" a.out
echo "ok: the exact name is chosen"

# D
set +e
"$MCPP" test --print-launch > d1.out 2> d1.err; rc1=$?
"$MCPP" test unit/ --print-launch > d2.out 2> d2.err; rc2=$?
"$MCPP" test unit/math --print-launch --message-format json > d3.out 2> d3.err; rc3=$?
"$MCPP" test unit/math --print-launch --no-run > d4.out 2> d4.err; rc4=$?
set -e
[ "$rc1" = 2 ] && grep -q "names no test" d1.err || fail "D: no pattern ($rc1)" d1.err
[ "$rc2" = 2 ] && grep -q "describes one test" d2.err || fail "D: several tests ($rc2)" d2.err
[ "$rc3" = 2 ] && grep -q "message-format json" d3.err || fail "D: json ($rc3)" d3.err
[ "$rc4" = 2 ] && grep -q -- "--no-run" d4.err || fail "D: --no-run ($rc4)" d4.err
echo "ok: the refusals"

# E
"$MCPP" --protocol-version > e.out 2>&1 || fail "E: --protocol-version" e.out
grep -q '"test-launch"' e.out || fail "E: test-launch is not stated" e.out
echo "ok: the feature is stated"

# A named runner is the same executable transformation for a test and a run.
# Python is a real host runner here, so the generated argv can also be executed.
$PY - <<'PY'
import json,sys
from pathlib import Path
Path('runner.py').write_text('import json,subprocess,sys\nwith open("named-invocations.txt","a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\nprint("NAMED RUNNER", flush=True)\nsys.exit(subprocess.call(sys.argv[1:]))\n')
with Path('mcpp.toml').open('a') as f:
    os_name={'linux':'linux','darwin':'macos','win32':'windows'}[sys.platform]
    f.write('\n[target.\'cfg(os = "'+os_name+'")\'.runners]\ndebuggee = '+json.dumps([sys.executable,str(Path('runner.py').resolve()),'{}'])+'\n')
PY
"$MCPP" test unit/math --runner debuggee --print-launch -- 'with spaces' > named.out 2> named.err \
    || fail "named test launch" named.err
$PY - named.out > named-run.out 2> named-run.err <<'PY' || fail "named launch execution" named-run.out named-run.err
import json,sys,os,subprocess
d=json.load(open(sys.argv[1]))['data']
assert d['via']=='runner', d
assert d['configuration']['runners']==['debuggee'], d
assert d['argv'][-1]=='with spaces', d
assert d['argv'][1].endswith('runner.py'), d
sys.exit(subprocess.call(d['argv'],cwd=d['cwd'],env={**os.environ,**d['env']}))
PY
grep -q 'NAMED RUNNER' named-run.out || fail "named runner did not run" named-run.out
grep -q 'last \[with spaces\]' named-run.out || fail "named runner changed arguments" named-run.out
rm named-invocations.txt
"$MCPP" test unit/math2 --runner debuggee -- direct > named-direct.out 2> named-direct.err \
    || fail "named runner direct test" named-direct.out named-direct.err
$PY - <<'PY' || fail "direct test skipped named runner" named-direct.out
import json
rows=[json.loads(line) for line in open('named-invocations.txt')]
assert len(rows)==1 and 'math2' in rows[0][0] and rows[0][-1]=='direct', rows
PY
"$MCPP" test unit/math2 --no-run --archive named-tests.tar.gz > named-archive-build.out 2> named-archive-build.err \
    || fail "test archive build" named-archive-build.out named-archive-build.err
rm named-invocations.txt
"$MCPP" test --archive named-tests.tar.gz --runner debuggee -- archived > named-archive.out 2> named-archive.err \
    || fail "named archive runner" named-archive.out named-archive.err
$PY - <<'PY' || fail "archive skipped named runner" named-archive.out
import json
rows=[json.loads(line) for line in open('named-invocations.txt')]
assert len(rows)==1 and 'math2' in rows[0][0] and rows[0][-1]=='archived', rows
PY
set +e
"$MCPP" test unit/math --runner absent --print-launch > named-missing.out 2> named-missing.err; rc1=$?
"$MCPP" test unit/math --runner debuggee --no-runner > named-conflict.out 2> named-conflict.err; rc2=$?
set -e
[ "$rc1" = 2 ] && grep -q "no runner named 'absent'" named-missing.err || fail "missing test runner ($rc1)" named-missing.err
[ "$rc2" = 2 ] && grep -q -- '--runner and --no-runner' named-conflict.err || fail "contradictory test runners ($rc2)" named-conflict.err
echo "ok: named test launch, execution, archive, argument preservation and refusal"

echo "OK"
