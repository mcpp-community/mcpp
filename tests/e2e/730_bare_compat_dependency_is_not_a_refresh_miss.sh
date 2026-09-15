#!/usr/bin/env bash
# requires: elf
# 730 -- a dependency the resolver reaches through the deprecated bare-name rung
# is not a descriptor miss for the index refresh decision (#648 L1).
#
# `ftxui = "6.1.9"` omits the namespace, which means mcpplibs; the package is
# published as compat.ftxui. The resolver tries compat after the exact miss and
# resolves it from disk. The refresh decision used to stop at the exact
# coordinate and asked for a network refresh, which `-v` shows offline as a
# decision suppressed by offline mode. Criteria, planned offline with `-v`:
#   A. the resolver takes the rung (its deprecation warning is printed), so the
#      fixture exercises the path at all;
#   B. no refresh decision for that dependency is suppressed by offline mode.
# Skipped when the local mcpplibs index has no compat descriptor to reach.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

INDEX_ROOT="${MCPP_HOME:-$HOME/.mcpp}/registry/data/mcpplibs/pkgs/c"
DESC=""
for cand in cjson argparse gtest ftxui; do
    if [ -f "$INDEX_ROOT/compat.$cand.lua" ]; then DESC="$cand"; break; fi
done
if [ -z "$DESC" ]; then
    echo "SKIP: no compat descriptor in the local mcpplibs index"
    exit 0
fi
VER=$(grep -o '\["[0-9][0-9.]*"\]' "$INDEX_ROOT/compat.$DESC.lua" | head -1 | tr -d '[]"')
[ -n "$VER" ] || fail "could not read a version of compat.$DESC"

mkdir -p "$TMP/app/src"
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
$DESC = "$VER"
EOF
echo 'int main() { return 0; }' > "$TMP/app/src/main.cpp"

cd "$TMP/app"
MCPP_OFFLINE=1 "$MCPP" emit build-database --format json -v > out.json 2> err.txt || true

grep -q "resolved to 'compat.$DESC' through the deprecated bare-name search" err.txt \
    || fail "A: the resolver did not take the bare-name rung for $DESC" err.txt
if grep -a "index: $DESC@$VER: offline mode" err.txt; then
    fail "B: the refresh decision asked for a network refresh of a dependency the resolver found" err.txt
fi
echo "PASS: 730 bare compat dependency ($DESC@$VER) is not a refresh miss"
