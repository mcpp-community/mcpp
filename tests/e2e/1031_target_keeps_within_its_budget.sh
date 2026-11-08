#!/usr/bin/env bash
# 1031 -- target/ keeps within its budget (mcpp#874).
#
# `[build] target_budget` states the bytes a project's target/ may hold. After
# every successful build over the budget, the build directories no
# configuration uses are removed whatever their age (the daily sweep keeps them
# for `[build] keep_days`), and when the ones in use still exceed it the build
# says so, naming the largest.
#
# Criteria:
#   A. A directory no configuration uses, written a minute ago, is removed by
#      the build after it when the budget is exceeded.
#   B. The configuration in use is kept, and the build warns that target/ is
#      over its budget in configurations still in use.
#   C. Without a budget the same directory is kept (keep_days).
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
cd "$TMP"
"$MCPP" new app > /dev/null
cd app
"$MCPP" build > a.log 2>&1 || fail "the build failed" a.log
cfg=$(dirname "$(dirname "$(find target -path '*/bin/app' -o -path '*/bin/app.exe' | head -1)")")
[ -d "$cfg" ] || fail "no configuration directory" a.log
stale="$(dirname "$cfg")/0000000000000000"
cp -R "$cfg" "$stale"

# C
printf '\n// c\n' >> src/main.cpp
"$MCPP" build > c.log 2>&1 || fail "C: the build failed" c.log
[ -d "$stale" ] || fail "C: without a budget a recent unused directory was removed" c.log
echo "ok: without a budget the recent directory is kept"

# A, B
printf '\n[build]\ntarget_budget = "1KiB"\n' >> mcpp.toml
printf '\n// a\n' >> src/main.cpp
"$MCPP" build > b.log 2>&1 || fail "the build with a budget failed" b.log
[ ! -d "$stale" ] || fail "A: the unused directory survived a build over budget" b.log
[ -d "$cfg" ] || fail "B: the configuration in use was removed" b.log
grep -q 'over \[build\] target_budget' b.log || fail "B: no warning for a target/ over budget in use" b.log
grep -q 'unsupported key' b.log && fail "B: target_budget is not a known key" b.log
echo "ok: over budget, the unused directory goes and the build says what remains"
echo "PASS: 1031 target/ keeps within its budget"
