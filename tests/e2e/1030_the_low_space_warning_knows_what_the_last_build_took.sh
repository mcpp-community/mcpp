#!/usr/bin/env bash
# 1030 -- the low-space warning knows what the configuration's last build took
# (mcpp#874).
#
# A build warns before it starts when the file system it writes to has less free
# space than it needs. The threshold was a fixed 2 GiB, so a workspace whose
# build writes 20 GB went ahead and failed at its link. A build now records what
# it took (`.mcpp-written` in its configuration's directory: the free space
# before it less the free space after it), and the next build warns below that,
# a quarter more, and never below 2 GiB.
#
# Criteria:
#   A. A build leaves `.mcpp-written` in its configuration's directory, a number.
#   B. With the record stating more than the file system has free, the next
#      build says so before it builds, naming what the last build took.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
cd "$TMP"
"$MCPP" new app > /dev/null
cd app
"$MCPP" build > a.log 2>&1 || fail "the build failed" a.log
rec=$(find target -name .mcpp-written | head -1)
[ -n "$rec" ] || { echo "SKIP: the file system reported no change in free space"; exit 77; }
grep -Eq '^[0-9]+$' "$rec" || fail "A: .mcpp-written is not a number" "$rec"
echo "ok: the build recorded what it took ($(cat "$rec") bytes)"
echo 1152921504606846976 > "$rec"    # 1 EiB: more than any file system here has free
printf '\n// changed\n' >> src/main.cpp
"$MCPP" build > b.log 2>&1 || fail "the second build failed" b.log
grep -q 'free where this build writes' b.log || fail "B: no warning before a build that needs more than is free" b.log
grep -q 'last build took' b.log || fail "B: the warning does not say what the last build took" b.log
echo "ok: the next build is warned"
echo "PASS: 1030 the low-space warning knows what the last build took"
