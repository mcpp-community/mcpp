#!/usr/bin/env bash
# `mcpp add foo@1.0.0` modifies mcpp.toml [dependencies].
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# Add a dep (we don't actually fetch — just verifies manifest mutation)
"$MCPP" add somedep@0.1.0 > /dev/null

# Check that mcpp.toml now has [dependencies] with somedep = "0.1.0"
grep -q '\[dependencies\]'    mcpp.toml || { cat mcpp.toml; echo "no [dependencies] section"; exit 1; }
grep -q '"somedep" = "0.1.0"' mcpp.toml || { cat mcpp.toml; echo "somedep version not set"; exit 1; }

# Add a second dep — should append, not duplicate the [dependencies] header
"$MCPP" add another@0.2.0 > /dev/null
header_count=$(grep -c '^\[dependencies\]$' mcpp.toml)
[[ "$header_count" == "1" ]] || { cat mcpp.toml; echo "[dependencies] header duplicated"; exit 1; }
grep -q '"another" = "0.2.0"' mcpp.toml || { cat mcpp.toml; echo "another not set"; exit 1; }

# Reject missing version
err=$("$MCPP" add bareword 2>&1) && { echo "expected error for missing version"; exit 1; }
[[ "$err" == *"version required"* ]] || { echo "wrong error: $err"; exit 1; }

echo "OK"
