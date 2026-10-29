#!/usr/bin/env bash
# requires: llvm python3
# 963 -- a target set builds every member and runs where it is reached
# (SPEC-015 §1-§3; design 2026-10-29 rev 2 §3).
#
# `--target openkal` names every row of the target table whose system is
# openkal: `x86_64-openkal` and `aarch64-openkal`. A command over a set acts
# once per member, each member a leg: the legs are planned one after another
# and built at once. `mcpp run` runs one leg, the first this machine reaches,
# its own architecture first; `mcpp test` builds every leg, runs the legs this
# machine reaches and reports the others built, not run; a command whose legs
# ran no test fails. On 2026.10.29.1 `--target openkal` is an unknown target.
#
# Criteria:
#   A. `mcpp build --target openkal` builds both legs, each under
#      target/<triple>/, and states one `Finished`.
#   B. `--message-format json`: the envelope's `data` is the first leg's
#      record with `legs` holding both, and every artifact event names its leg.
#   C. `mcpp run --target openkal` runs this machine's leg and says which.
#   D. `mcpp test --target openkal` runs this machine's leg, reports the other
#      built and not run, and exits 0.
#   E. With an execution host named for each leg that does not exist, `mcpp
#      test --target openkal` runs no test on any leg and exits 1, saying so.
#   F. `mcpp self env --target openkal --format json` reports both legs.
#   G. Refusals: `none` (no environment), `linux-msvc` (no member), and a set
#      in `[build] target`.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64;  OTHER=aarch64 ;;
    aarch64|arm64) HOST_ARCH=aarch64; OTHER=x86_64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
HT="$HOST_ARCH-openkal"
OT="$OTHER-openkal"

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
int main() { std::println("image of the set"); return 0; }
CPP
cat > tests/smoke.cpp <<'CPP'
import std;
int main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }
CPP

# A
"$MCPP" build --target openkal > a.log 2>&1 || fail "A: mcpp build --target openkal" a.log
for t in "$HT" "$OT"; do
    ls target/"$t"/*/bin/app.oke > /dev/null 2>&1 || fail "A: no image for $t" a.log
done
[ "$(grep -c 'Finished' a.log)" = 1 ] || fail "A: not exactly one Finished" a.log
echo "ok: a target set builds every member"

# B
"$MCPP" build --target openkal --message-format json > b.json 2> b.err || fail "B: json build" b.err b.json
python3 - b.json x86_64-openkal aarch64-openkal <<'PY' || fail "B: the envelope or the events" b.json
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
env = lines[-1]
assert env["kind"] == "mcpp.build", env["kind"]
legs = env["data"]["legs"]
assert len(legs) == 2, legs
triples = sorted(l["configuration"]["triple"] for l in legs)
assert triples == sorted(sys.argv[2:]), triples
assert env["data"]["configuration"]["triple"] == sys.argv[2], env["data"]["configuration"]
assert env["data"]["status"] == "ok", env["data"]["status"]
arts = [l for l in lines[:-1] if l.get("event") == "artifact"]
assert arts and all("triple" in a for a in arts), arts
PY
echo "ok: the envelope lists the legs, and each artifact event names its leg"

# C
"$MCPP" run --target openkal > c.log 2>&1 || fail "C: mcpp run --target openkal" c.log
grep -q "image of the set" c.log || fail "C: the program did not run" c.log
grep -q "Leg.*$HT" c.log || fail "C: the run does not say which leg" c.log
echo "ok: run runs this machine's leg"

# D
"$MCPP" test --target openkal > d.log 2>&1 || fail "D: mcpp test --target openkal" d.log
grep -q "smoke ... ok" d.log || fail "D: this machine's leg did not pass" d.log
grep -q "built, not run" d.log || fail "D: the other leg is not reported built, not run" d.log
grep -q "legs result" d.log || fail "D: no result over the legs" d.log
echo "ok: test runs the legs this machine reaches"

# E
rc=0
MCPP_TARGET_X86_64_OPENKAL_RUN_UNDER=/nonexistent/execution-host \
MCPP_TARGET_AARCH64_OPENKAL_RUN_UNDER=/nonexistent/execution-host \
    "$MCPP" test --target openkal > e.log 2>&1 || rc=$?
[ "$rc" = 1 ] || fail "E: a test over legs that ran nothing exited $rc, not 1" e.log
grep -q "no test ran on any of the 2 legs" e.log || fail "E: the failure does not say so" e.log
echo "ok: no test run on any leg is a failure"

# F
"$MCPP" self env --target openkal --format json > f.json 2> f.err || fail "F: self env" f.err f.json
python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))["data"]; assert len(d["legs"]) == 2, d' f.json \
    || fail "F: self env does not report both legs" f.json
echo "ok: self env reports each leg"

# G
rc=0; "$MCPP" build --target none > g1.log 2>&1 || rc=$?
[ "$rc" = 2 ] && grep -q "none-elf" g1.log || fail "G: \`none\` is not refused naming the environments" g1.log
rc=0; "$MCPP" build --target linux-msvc > g2.log 2>&1 || rc=$?
[ "$rc" = 2 ] && grep -q "names no target" g2.log || fail "G: an empty set is not refused" g2.log
mkdir -p "$TMP/bt/src" && cd "$TMP/bt"
printf '[package]\nname = "bt"\nversion = "0.1.0"\n[build]\ntarget = "openkal"\n' > mcpp.toml
printf 'int main() {}\n' > src/main.cpp
rc=0; "$MCPP" build > g3.log 2>&1 || rc=$?
[ "$rc" != 0 ] && grep -q "names a target set" g3.log || fail "G: a set in [build] target is not refused" g3.log
echo "ok: sets are refused where they name nothing, and in [build] target"

echo "PASS: 963 a target set builds every member and runs where it is reached"
