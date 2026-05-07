#!/usr/bin/env bash
# Verify --help and --version
set -e

out=$("$MCPP" --version)
# Version must match `mcpp <SemVer>` — don't pin a specific version here
# so this test doesn't break on every release bump.
[[ "$out" =~ ^mcpp\ [0-9]+\.[0-9]+\.[0-9]+ ]] \
    || { echo "Bad version output: $out"; exit 1; }

out=$("$MCPP" --help)
[[ "$out" == *"Usage:"* ]] || { echo "--help missing 'Usage:' section"; exit 1; }
[[ "$out" == *"mcpp new"* ]] || { echo "--help missing 'mcpp new'"; exit 1; }
[[ "$out" == *"mcpp build"* ]] || { echo "--help missing 'mcpp build'"; exit 1; }

# Unknown command exit code (127) — must capture rc explicitly under `set -e`
rc=0
"$MCPP" doesnotexist >/dev/null 2>&1 || rc=$?
[[ $rc -eq 127 ]] || { echo "expected exit 127 for unknown command, got $rc"; exit 1; }

echo "OK"
