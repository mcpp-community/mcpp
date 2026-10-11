#!/usr/bin/env bash
# requires:
# 860 -- the build programs of a workspace are compiled at the same time, up to
# the job count, and run one after another; the failure reported is the first
# in the order they run in.
#
# #748 (B2), mcpp#797. A member's program runs after the programs of the
# members it depends on, and that order is kept. Their compiles have no order:
# every program of the build is compiled in one build of the build programs'
# graph, whose ninja runs up to the job count at once.
#
# Criteria:
#   A. With `-j 4`, the compile intervals of three independent programs
#      overlap. The test compares the start and the end ninja records for each
#      program's object in the graph's `.ninja_log`, not the time the build took.
#   B. With `-j 1`, they do not: each compile ends before the next begins.
#   C. When two programs fail to compile, the failure reported is the first in
#      the order the programs run in, whichever compile finished first. The
#      program that runs second fails at once, and the one that runs first
#      after a slow parse; at `-j 4` the second finishes first.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws"
cd "$TMP/ws"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["pa", "pb", "pc"]
EOF
for m in pa pb pc; do
    mkdir -p $m/src
    cat > $m/mcpp.toml <<EOF
[package]
name    = "$m"
version = "0.1.0"

[targets.$m]
kind = "bin"
main = "src/main.cpp"
EOF
    printf 'int main() { return 0; }\n' > $m/src/main.cpp
    # Headers that take a while to parse, so that a compile is long enough for
    # another to be under way at the same time.
    cat > $m/build.mcpp <<EOF
#include <algorithm>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <string>
import mcpp;
int main() {
    std::regex re("a+b");
    mcpp::define("T860_${m}=1");
    return std::regex_match("aab", re) ? 0 : 1;
}
EOF
done

# The start and the end, in milliseconds, ninja recorded for the object of
# member $2's program in the build programs' graph. ($1, the build's log, is
# kept for the failure message.)
host_log() { find target/.build-mcpp/host -name .ninja_log | head -1; }
# The names of the programs that ran, one per line, in the order the lines came.
ran_lines() { grep -E "^ *build\.mcpp p[abc] .*ran [0-9]" "$1" | sed -E 's/^ *build\.mcpp (p[abc]) .*/\1/'; }
span() { awk -F'\t' -v m="$2" '$4 ~ ("^obj/" m "-[0-9a-f]+/.*build\\.mcpp\\.o$") { print $1, $2; exit }' "$(host_log)"; }

# A
"$MCPP" build --workspace -j 4 > a.log 2>&1 || fail "A: the workspace did not build at -j 4" a.log
read -r sa ea <<< "$(span a.log pa)"
read -r sb eb <<< "$(span a.log pb)"
read -r sc ec <<< "$(span a.log pc)"
for v in "$sa" "$ea" "$sb" "$eb" "$sc" "$ec"; do
    [ -n "$v" ] || fail "A: the build programs' .ninja_log does not record every compile" a.log "$(host_log)"
done
latest_start=$sa; [ "$sb" -gt "$latest_start" ] && latest_start=$sb; [ "$sc" -gt "$latest_start" ] && latest_start=$sc
earliest_end=$ea; [ "$eb" -lt "$earliest_end" ] && earliest_end=$eb; [ "$ec" -lt "$earliest_end" ] && earliest_end=$ec
[ "$latest_start" -lt "$earliest_end" ] \
    || fail "A: the compiles did not overlap at -j 4 (starts $sa $sb $sc, ends $ea $eb $ec)" a.log

# The order the programs run in: what the lines of a build say.
order=$(ran_lines a.log | tr '\n' ' ')
[ "$(echo $order | wc -w | tr -d ' ')" = 3 ] || fail "A: expected three programs to run, got: $order" a.log

# B
rm -rf target pa/target pb/target pc/target
"$MCPP" build --workspace -j 1 > b.log 2>&1 || fail "B: the workspace did not build at -j 1" b.log
starts=()
for m in pa pb pc; do
    read -r s e <<< "$(span b.log $m)"
    [ -n "$s" ] && [ -n "$e" ] || fail "B: the build programs' .ninja_log does not record the compile of $m" b.log "$(host_log)"
    starts+=("$s:$e")
done
sorted=$(printf '%s\n' "${starts[@]}" | sort -n)
prev_end=0
while IFS=: read -r s e; do
    [ "$s" -ge "$prev_end" ] || fail "B: two compiles overlapped at -j 1 ($sorted)" b.log
    prev_end=$e
done <<< "$sorted"
order1=$(ran_lines b.log | tr '\n' ' ')
[ "$order" = "$order1" ] || fail "B: the programs ran in another order at -j 1 ($order1) than at -j 4 ($order)" b.log

# C. The one that runs first fails after the headers are parsed, the one that
# runs second at once.
first=$(echo $order | awk '{ print $1 }')
second=$(echo $order | awk '{ print $2 }')
cat > $first/build.mcpp <<EOF
#include <regex>
#include <map>
#error T860-failure-in-$first
int main() { return 0; }
EOF
cat > $second/build.mcpp <<EOF
#error T860-failure-in-$second
int main() { return 0; }
EOF
rm -rf target pa/target pb/target pc/target
if "$MCPP" build --workspace -j 4 > c4.log 2>&1; then fail "C: the workspace built although two programs do not compile" c4.log; fi
grep -q "T860-failure-in-$first" c4.log \
    || fail "C: the failure reported at -j 4 is not the one of $first, the first to run" c4.log
if grep -q "T860-failure-in-$second" c4.log; then
    fail "C: the failure of $second, which runs after $first, was reported beside it" c4.log
fi
rm -rf target pa/target pb/target pc/target
if "$MCPP" build --workspace -j 1 > c1.log 2>&1; then fail "C: the workspace built although two programs do not compile (-j 1)" c1.log; fi
grep -q "T860-failure-in-$first" c1.log || fail "C: the failure reported at -j 1 is not the one of $first" c1.log

echo "PASS: 860_a_workspaces_programs_compile_at_the_same_time"
