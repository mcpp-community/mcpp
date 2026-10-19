#!/usr/bin/env bash
# requires:
# 38_self_config_mirror.sh — configure xlings mirror through mcpp self config.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
"$MCPP" self config > "$TMP/query.log" 2>&1 || {
    cat "$TMP/query.log"
    echo "FAIL: reading fresh configuration failed"
    exit 1
}
[[ ! -e "$MCPP_HOME" ]] || { echo "FAIL: reading configuration initialized the home"; exit 1; }

# Choosing a mirror creates local configuration, never a sandbox bootstrap.
"$MCPP" self config --mirror auto > "$TMP/auto.log" 2>&1 || {
    cat "$TMP/auto.log"; echo "FAIL: selecting auto failed"; exit 1; }
grep -q '"mirror": "auto"' "$MCPP_HOME/registry/.xlings.json" || {
    cat "$MCPP_HOME/registry/.xlings.json"
    echo "FAIL: auto should restore adaptive region detection"
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
