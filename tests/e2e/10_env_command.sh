#!/usr/bin/env bash
# `mcpp env` initializes $MCPP_HOME and prints expected layout.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

# First invocation should silently init the directory tree.
out=$("$MCPP" self env 2>&1)

# Verify directory tree
[[ -d "$MCPP_HOME/bin" ]]      || { echo "missing bin/"; exit 1; }
[[ -d "$MCPP_HOME/registry" ]] || { echo "missing registry/"; exit 1; }
[[ -d "$MCPP_HOME/bmi" ]]      || { echo "missing bmi/"; exit 1; }
[[ -d "$MCPP_HOME/cache" ]]    || { echo "missing cache/"; exit 1; }
[[ -f "$MCPP_HOME/config.toml" ]]            || { echo "missing config.toml"; exit 1; }
[[ -f "$MCPP_HOME/registry/.xlings.json" ]]  || { echo "missing seeded .xlings.json"; exit 1; }
[[ -x "$MCPP_HOME/registry/bin/xlings" ]] || { echo "xlings binary not acquired"; exit 1; }

# Verify seeded .xlings.json contains mcpplibs and NOT awesome
grep -q '"name": "mcpplibs"' "$MCPP_HOME/registry/.xlings.json" || {
    echo "seeded .xlings.json missing mcpplibs"; exit 1; }
if grep -q '"name": "awesome"' "$MCPP_HOME/registry/.xlings.json"; then
    echo "seeded .xlings.json should NOT contain awesome"; exit 1
fi

# Output should mention key paths (use grep — [[ pattern match struggles with multi-line + parens)
echo "$out" | grep -q 'MCPP_HOME'     || { echo "no MCPP_HOME in env output"; exit 1; }
echo "$out" | grep -q 'xlings binary' || { echo "no xlings binary in env output"; exit 1; }
echo "$out" | grep -q 'mcpplibs'    || { echo "no mcpplibs in env output"; exit 1; }

echo "OK"
