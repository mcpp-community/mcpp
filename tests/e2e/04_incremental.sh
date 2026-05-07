#!/usr/bin/env bash
# Incremental: a no-op rebuild does no work; touching main.cpp recompiles only it
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new inc
cd inc
"$MCPP" build > /dev/null

# 1. Empty rebuild is fast (Ninja: nothing to do)
out=$("$MCPP" build 2>&1)
[[ "$out" == *"Finished"* || "$out" == *"Build OK"* ]] || { echo "rebuild failed: $out"; exit 1; }

# 2. Touch main.cpp; only main.o + link should rebuild
sleep 1 # ensure mtime change is observed
touch src/main.cpp
out=$("$MCPP" build --verbose 2>&1)
# Ninja prints "[N/M] /path/g++ ... -c .../main.cpp ..."
echo "$out" | grep -q "main.cpp" || { echo "main.cpp not rebuilt: $out"; exit 1; }
# std.o should NOT be re-staged
echo "$out" | grep -q "std.cc"   && { echo "std module re-built when it shouldn't be"; exit 1; }

echo "OK"
