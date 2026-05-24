#!/usr/bin/env bash
# requires:
# 46_self_config_mirror_fresh_seed.sh — `mcpp self config --mirror X` on a
# fresh MCPP_HOME must seed .xlings.json with X from the very first write,
# so the immediately-following sandbox bootstrap (patchelf / ninja
# download) uses the user's chosen mirror instead of the historical CN
# default.
#
# This is the user-visible fix for the "fresh-install mirror config hangs
# on overseas networks" report: previously the seed always wrote
# `"mirror": "CN"` regardless of --mirror, and bootstrap downloads went
# through an unreachable CN proxy before xlings::config_set_mirror had a
# chance to change the value.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/fresh"

# Sanity: file doesn't exist yet — we're truly fresh.
[[ ! -f "$MCPP_HOME/registry/.xlings.json" ]] || {
    echo "FAIL: precondition — registry/.xlings.json already exists"; exit 1; }

"$MCPP" self config --mirror GLOBAL > "$TMP/out.log" 2>&1 || {
    cat "$TMP/out.log"
    echo "FAIL: 'self config --mirror GLOBAL' exited non-zero on fresh MCPP_HOME"
    exit 1
}

# After the command the seeded .xlings.json must reflect GLOBAL, NOT the
# default CN. This is the property the fix guarantees on the seed path —
# the OLD code seeded "CN" first and only switched to GLOBAL via a
# post-bootstrap xlings call, leaving the patchelf/ninja download going
# through CN.
grep -q '"mirror": "GLOBAL"' "$MCPP_HOME/registry/.xlings.json" || {
    cat "$MCPP_HOME/registry/.xlings.json"
    echo "FAIL: .xlings.json mirror should be GLOBAL after fresh-init --mirror"
    exit 1
}

# Round-trip back to CN — also via --mirror — exercises the post-init
# path (xlings config --mirror) since .xlings.json now exists.
"$MCPP" self config --mirror CN > "$TMP/cn.log" 2>&1 || {
    cat "$TMP/cn.log"; echo "FAIL: switching back to CN failed"; exit 1; }
grep -q '"mirror": "CN"' "$MCPP_HOME/registry/.xlings.json" || {
    cat "$MCPP_HOME/registry/.xlings.json"
    echo "FAIL: round-trip to CN did not stick"
    exit 1
}

echo "OK"
