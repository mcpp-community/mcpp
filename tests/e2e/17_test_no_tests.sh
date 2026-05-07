#!/usr/bin/env bash
# Project with no tests/ → `mcpp test` says "no tests found" and exits 0.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp
rm -rf tests       # remove the auto-generated test_smoke.cpp

out=$("$MCPP" test 2>&1)
echo "$out" | grep -q 'no tests found' || { echo "missing 'no tests found': $out"; exit 1; }

echo "OK"
