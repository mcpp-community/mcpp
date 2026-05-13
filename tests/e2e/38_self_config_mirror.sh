#!/usr/bin/env bash
# 38_self_config_mirror.sh — configure xlings mirror through mcpp self config.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

"$MCPP" self env > "$TMP/env.log" 2>&1 || {
    cat "$TMP/env.log"
    echo "FAIL: self env failed"
    exit 1
}

grep -q '"mirror": "CN"' "$MCPP_HOME/registry/.xlings.json" || {
    cat "$MCPP_HOME/registry/.xlings.json"
    echo "FAIL: default mirror should be CN"
    exit 1
}

"$MCPP" self config --mirror GLOBAL > "$TMP/global.log" 2>&1 || {
    cat "$TMP/global.log"
    echo "FAIL: self config --mirror GLOBAL failed"
    exit 1
}
grep -q '"mirror": "GLOBAL"' "$MCPP_HOME/registry/.xlings.json" || {
    cat "$MCPP_HOME/registry/.xlings.json"
    echo "FAIL: mirror should be GLOBAL"
    exit 1
}

"$MCPP" self config --mirror cn > "$TMP/cn.log" 2>&1 || {
    cat "$TMP/cn.log"
    echo "FAIL: self config --mirror cn failed"
    exit 1
}
grep -q '"mirror": "CN"' "$MCPP_HOME/registry/.xlings.json" || {
    cat "$MCPP_HOME/registry/.xlings.json"
    echo "FAIL: mirror should be CN"
    exit 1
}

if "$MCPP" self config --mirror BAD > "$TMP/bad.log" 2>&1; then
    cat "$TMP/bad.log"
    echo "FAIL: invalid mirror unexpectedly succeeded"
    exit 1
fi
grep -q "invalid mirror" "$TMP/bad.log" || {
    cat "$TMP/bad.log"
    echo "FAIL: invalid mirror diagnostic missing"
    exit 1
}

echo "OK"
