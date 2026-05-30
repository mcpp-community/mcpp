#!/usr/bin/env bash
# requires:
# Verify --help and --version
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

out=$("$MCPP" --version)
# Version must match `mcpp <SemVer>` — don't pin a specific version here
# so this test doesn't break on every release bump.
[[ "$out" =~ ^mcpp\ [0-9]+\.[0-9]+\.[0-9]+ ]] \
    || { echo "Bad version output: $out"; exit 1; }
expected="$(awk -F '"' '/^version[[:space:]]*=/{print $2; exit}' "$ROOT/mcpp.toml")"
[[ -n "$expected" ]] || { echo "Failed to read mcpp.toml package version"; exit 1; }
[[ "$out" == "mcpp $expected"* ]] \
    || { echo "Version mismatch: mcpp.toml=$expected, --version='$out'"; exit 1; }

out=$("$MCPP" --help)
[[ "$out" == *"Usage:"* ]] || { echo "--help missing 'Usage:' section"; exit 1; }
[[ "$out" == *"mcpp new"* ]] || { echo "--help missing 'mcpp new'"; exit 1; }
[[ "$out" == *"mcpp build"* ]] || { echo "--help missing 'mcpp build'"; exit 1; }

# Unknown command exit code (127) — must capture rc explicitly under `set -e`
rc=0
"$MCPP" doesnotexist >/dev/null 2>&1 || rc=$?
[[ $rc -eq 127 ]] || { echo "expected exit 127 for unknown command, got $rc"; exit 1; }

echo "OK"
