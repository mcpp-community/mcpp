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

echo "OK"
