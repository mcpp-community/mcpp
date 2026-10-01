#!/usr/bin/env bash
# requires: pack python3
# 871 -- `mcpp pack` states the build it performs as `mcpp build` states one,
# and every command that compiles runs at most `[build] jobs` compiles at once
# (pack drive and selection design 2026-10-01, A; mcpp#753).
#
# Each command assembled its own ninja options. `mcpp build` attached the
# report and passed the job count; `mcpp pack` did neither, so a release job
# whose pack recompiled mcpp showed `Planning` for six minutes and no
# `Compiling` line, and `mcpp test` and `mcpp pack` ran ninja's default number
# of jobs whatever `[build] jobs` said. The backend now takes both from the plan
# and the open report.
#
# Criteria:
#   A. A cold `mcpp pack --format tar` writes a `Compiling` line for the
#      package and `Finished` before `Packing`.
#   B. Under `--message-format json`, standard output is one JSON document.
#   C. With `[build] jobs = 1`, no two compile steps overlap in `.ninja_log`
#      under `mcpp build`, `mcpp test` and `mcpp pack`. Four units of about a
#      third of a second each overlap under ninja's default on any machine
#      with two cores.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
cd "$TMP"

mkdir -p app/src app/tests
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
jobs = 1

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
# Four translation units whose compile takes a measurable time: a constant
# evaluation of a few hundred thousand steps each, in two nested loops so that
# no loop exceeds GCC's per-loop limit (262144 iterations) and the whole stays
# within clang's step limit.
for i in 1 2 3 4; do
    cat > app/src/u$i.cpp <<EOF
constexpr unsigned long long spin$i() {
    unsigned long long x = $i;
    for (unsigned k = 0; k < 600; ++k)
        for (unsigned j = 0; j < 600; ++j) x = x * 6364136223846793005ull + 1442695040888963407ull;
    return x;
}
unsigned long long value$i() { constexpr auto v = spin$i(); return v; }
EOF
done
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
unsigned long long value1(); unsigned long long value2();
unsigned long long value3(); unsigned long long value4();
int main() { std::printf("%llu\n", value1() ^ value2() ^ value3() ^ value4()); }
EOF
cat > app/tests/t.cpp <<'EOF'
int main() { return 0; }
EOF
cd app

# no_overlap <label>: the compile steps of the newest run in `.ninja_log`
# (start and end in ms, one line per output) never run at the same time.
no_overlap() {
    local log
    log=$(find target -name .ninja_log -newer "$TMP/stamp" | head -1)
    [ -n "$log" ] || fail "$1: no .ninja_log was written"
    awk -F'\t' -v label="$1" '
        NR > 1 && $4 ~ /\.o(bj)?$/ { s[n] = $1; e[n] = $2; o[n] = $4; n++ }
        END {
            if (n < 4) { print "FAIL: " label ": expected at least four compiles in .ninja_log, found " n; exit 1 }
            for (i = 0; i < n; i++) for (j = i + 1; j < n; j++)
                if (s[i] < e[j] && s[j] < e[i]) {
                    print "FAIL: " label ": " o[i] " [" s[i] "," e[i] "] overlaps " o[j] " [" s[j] "," e[j] "]"
                    exit 1
                }
        }' "$log" || exit 1
}

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" pack --format tar > a.log 2>&1 || fail "A: pack failed" a.log
grep -q 'Compiling app v0.1.0' a.log || fail "A: the pack's build wrote no Compiling line" a.log
fin=$(grep -n 'Finished' a.log | head -1 | cut -d: -f1)
pk=$(grep -n 'Packing' a.log | head -1 | cut -d: -f1)
[ -n "$fin" ] && [ -n "$pk" ] && [ "$fin" -lt "$pk" ] \
    || fail "A: Finished (line ${fin:-none}) does not precede Packing (line ${pk:-none})" a.log
echo "ok: A, the pack states its build and Finished precedes Packing"

# ── B ──────────────────────────────────────────────────────────────────────
rm -rf target
"$MCPP" pack --format tar --message-format json > b.json 2> b.err || fail "B: pack failed" b.err
python3 -c 'import json, sys; json.load(open(sys.argv[1]))' b.json \
    || fail "B: standard output is not one JSON document" b.json b.err
echo "ok: B, the JSON output is one document"

# ── C ──────────────────────────────────────────────────────────────────────
rm -rf target; touch "$TMP/stamp"; sleep 1
"$MCPP" build > c1.log 2>&1 || fail "C: build failed" c1.log
no_overlap "C build"
rm -rf target; touch "$TMP/stamp"; sleep 1
"$MCPP" test > c2.log 2>&1 || fail "C: test failed" c2.log
no_overlap "C test"
rm -rf target; touch "$TMP/stamp"; sleep 1
"$MCPP" pack --format tar > c3.log 2>&1 || fail "C: pack failed" c3.log
no_overlap "C pack"
echo "ok: C, build, test and pack each run one compile at a time under jobs = 1"

echo "PASS: 871_a_pack_states_its_build_and_every_drive_takes_the_job_count"
