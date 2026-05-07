#!/usr/bin/env bash
# 27_self_contained_home.sh — verifies mcpp's self-contained home behaviour.
#
# Without MCPP_HOME set, mcpp resolves its home from the binary's location:
# binary at <ROOT>/bin/mcpp ⇒ MCPP_HOME = <ROOT>. This is what release
# tarballs (and `xlings install mcpp`) rely on so the unpacked tree IS the
# fully-populated mcpp home — no env var, no shell config required.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Lay out a release-style tree: <ROOT>/bin/mcpp
ROOT="$TMP/release"
mkdir -p "$ROOT/bin"
cp "$MCPP" "$ROOT/bin/mcpp"

# Run mcpp from that tree with MCPP_HOME unset. `env -u MCPP_HOME` strips
# the var even if the parent shell exported it.
out=$(env -u MCPP_HOME "$ROOT/bin/mcpp" self env 2>&1)

# The reported MCPP_HOME must be the binary's parent dir, NOT $HOME/.mcpp.
echo "$out" | grep -q "MCPP_HOME *= *$ROOT" || {
    echo "mcpp did not self-locate to $ROOT"; echo "--- output:"; echo "$out"; exit 1; }

# And it must have actually populated the layout there.
[[ -d "$ROOT/registry" ]]            || { echo "missing registry/"; exit 1; }
[[ -f "$ROOT/config.toml" ]]         || { echo "missing config.toml"; exit 1; }
[[ -x "$ROOT/bin/xlings" ]]          || { echo "xlings not acquired into $ROOT/bin"; exit 1; }

# Explicit env var must still win when set.
ALT="$TMP/explicit-home"
out2=$(MCPP_HOME="$ALT" "$ROOT/bin/mcpp" self env 2>&1)
echo "$out2" | grep -q "MCPP_HOME *= *$ALT" || {
    echo "MCPP_HOME env var did not override binary-relative resolution"; echo "$out2"; exit 1; }

echo "OK"
