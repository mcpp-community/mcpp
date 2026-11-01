#!/usr/bin/env bash
# requires: gcc
# 1005 -- `clean --stale` tells the truth about what it kept (mcpp#881), and
# its window of "recent" is the sweep's: `[build] keep_days`.
#
# Criteria:
#   A. With an unrecorded directory written a minute ago, the summary says one
#      directory was kept and within what window, and does not say every
#      directory is recorded as current.
#   B. The default window is `[build] keep_days` (3 days): a directory written
#      two days ago is kept by default, and removed with `--older-than 1d`.
#   C. With nothing unrecorded the summary is the one it always was.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p app/src && cd app
printf '[package]\nname = "app"\nversion = "0.1.0"\n' > mcpp.toml
printf 'int main() { return 0; }\n' > src/main.cpp
"$MCPP" build > build.log 2>&1 || fail "the build failed" build.log
triple=$(dirname "$(ls -d target/*/*/ | grep -E '/[0-9a-f]{16}/$' | head -1)")

# C
"$MCPP" clean --stale --dry-run > c.log 2>&1 || fail "clean --stale failed" c.log
grep -q 'every fingerprint directory is recorded as current' c.log || fail "the plain summary changed" c.log

# A
mkdir -p "$triple/deadbeefdeadbeef" && printf 'rule\n' > "$triple/deadbeefdeadbeef/build.ninja"
"$MCPP" clean --stale --dry-run > a.log 2>&1 || fail "clean --stale failed" a.log
grep -q 'kept    .*deadbeefdeadbeef' a.log || fail "the kept directory is not listed" a.log
! grep -q 'every fingerprint directory is recorded as current' a.log || fail "the summary still says every directory is current" a.log
grep -q '1 directory not in use kept, written within the last 3d' a.log || fail "the summary does not say what was kept" a.log

# B
touch -d '2 days ago' "$triple/deadbeefdeadbeef/build.ninja" "$triple/deadbeefdeadbeef"
"$MCPP" clean --stale > b0.log 2>&1 || fail "clean --stale failed" b0.log
[ -d "$triple/deadbeefdeadbeef" ] || fail "a directory written two days ago was removed under the default window" b0.log
"$MCPP" clean --stale --older-than 1d > b1.log 2>&1 || fail "clean --stale --older-than 1d failed" b1.log
[ ! -d "$triple/deadbeefdeadbeef" ] || fail "--older-than 1d kept a directory written two days ago" b1.log
echo "OK"
