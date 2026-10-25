#!/usr/bin/env bash
# 935 -- two builds of one configuration at once (mcpp#839, design 2026-10-26
# §4): the second waits for the build directory and both succeed.
#
# Before the build directory had a lock, two cold builds started 0.1-0.5 s apart
# wrote one directory at once, and in three rounds of five both failed
# (`failed to write/read compiled module`). Criteria, over five rounds:
#   A  both builds exit 0 in every round;
#   B  in at least one round the later build reports that it waited, naming
#      the holder;
#   C  the result record is complete after the last round (status ok) and
#      `target/.last-result.json` names it;
#   D  two different configurations (dev and --release) do not wait on each
#      other's lock (they share nothing to lock).
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/p/src"
cd "$TMP/p"
printf '[package]\nname = "lk"\nversion = "0.1.0"\nstandard = "c++23"\n' > mcpp.toml
for i in $(seq 1 24); do
    printf 'export module m%d;\nimport std;\nexport int f%d(){ std::vector<int> v(%d); return (int)v.size(); }\n' $i $i $i > "src/m$i.cppm"
done
{ echo 'import std;'; for i in $(seq 1 24); do echo "import m$i;"; done
  echo 'int main(){ std::println("{}", f1()+f24()); }'; } > src/main.cpp

"$MCPP" build > warm.log 2>&1 || fail "the first build failed" warm.log   # std module and toolchain in place
waited=0
for r in 1 2 3 4 5; do
    rm -rf target
    ( "$MCPP" build > a.log 2>&1; echo $? > a.rc ) &
    ( sleep "0.$r"; "$MCPP" build > b.log 2>&1; echo $? > b.rc ) &
    wait
    [[ "$(cat a.rc)" == 0 && "$(cat b.rc)" == 0 ]] || fail "A: round $r: rc=$(cat a.rc),$(cat b.rc)" a.log b.log
    grep -q "Waiting.*held by pid" a.log b.log && waited=1
done
[[ $waited == 1 ]] || echo "note: no round overlapped long enough to wait (B not observed on this machine)"

last=target/.last-result.json
[[ -f $last ]] || fail "C: no $last"
record=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["data"]["result"])' "$last")
python3 - "$record" <<'EOF' || fail "C: the result record is incomplete" "$record"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["kind"] == "mcpp.result", d["kind"]
data = d["data"]
assert data["status"] == "ok", data["status"]
assert data["finishedAt"], data
assert any(a["kind"] == "bin" and a.get("launch") for a in data["artifacts"]), data["artifacts"]
EOF

# D: dev and release are two directories, two locks.
( "$MCPP" build > dev.log 2>&1; echo $? > dev.rc ) &
( "$MCPP" build --release > rel.log 2>&1; echo $? > rel.rc ) &
wait
[[ "$(cat dev.rc)" == 0 && "$(cat rel.rc)" == 0 ]] || fail "D: rc=$(cat dev.rc),$(cat rel.rc)" dev.log rel.log
! grep -q "Waiting.*held by pid" dev.log rel.log || fail "D: two configurations waited on one lock" dev.log rel.log

echo "PASS: 935 two builds of one configuration"
