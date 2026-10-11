#!/usr/bin/env bash
# requires: gcc
# 798_a_rule_applies_through_a_source_it_claims.sh — mcpp#715.
#
# A feature activates a rule for a consumer; whether the rule has anything to
# do there is answered by the consumer's sources. Until 2026.9.27.1 the engine
# synthesised a build program for every active rule, so a package that enabled
# a rule feature only to import its module -- no device source, no
# `build.mcpp` -- compiled and ran a program that could only report "nothing
# to do", and printed `Rules` for it, on every configure.
#
# Two legs over examples/12's rule package, which is the engine's own
# reference rule and needs no network:
#   1. a consumer with a `.toy` device source and no `build.mcpp`: the rule is
#      reported, the synthesised program runs it, and the kernel reaches the
#      program (the direction the fix must not break);
#   2. a consumer that activates the same feature with no `.toy` source: no
#      `Rules` line and no synthesised program.
set -e

SRC="$(cd "$(dirname "$0")/../.." && pwd)/examples/12-a-new-device-language"
[[ -d "$SRC" ]] || { echo "FAIL: $SRC is missing"; exit 1; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cp -r "$SRC" "$TMP/ex"
find "$TMP/ex" -maxdepth 3 -type d -name target -exec rm -rf {} + 2>/dev/null || true
find "$TMP/ex" -maxdepth 3 -name mcpp.lock -delete 2>/dev/null || true

# ── 1. a claimed source: the rule runs through the synthesised program ──────
cd "$TMP/ex/app"
rm -f build.mcpp
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: leg 1 did not build"; exit 1; }
grep -q 'Rules.*example.rules.toy' b1.log || {
    cat b1.log; echo "FAIL: leg 1 did not report the rule it applied"; exit 1; }
find target -name build.mcpp | grep -q . || {
    echo "FAIL: leg 1 synthesised no build program"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == *"= 42"* ]] || {
    echo "FAIL: leg 1: the kernel did not reach the program: '$out'"; exit 1; }

# ── 2. no claimed source: nothing is synthesised ───────────────────────────
mkdir -p "$TMP/ex/importonly/src"
cd "$TMP/ex/importonly"
cat > mcpp.toml <<'EOF'
[package]
name    = "importonly"
version = "0.1.0"

[language]
standard = "c++23"

[dependencies]
example.rules-toy = { path = "../rules-toy", features = ["rules-toy"] }

[build]
sources = ["src/*.cpp"]
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("IMPORT_ONLY_OK\n"); }
EOF
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: leg 2 did not build"; exit 1; }
if grep -q 'Rules' b2.log; then
    cat b2.log; echo "FAIL: leg 2 reported a rule that claims none of its sources"; exit 1
fi
if find target -name build.mcpp | grep -q .; then
    find target -name build.mcpp
    echo "FAIL: leg 2 synthesised a build program with nothing to do"; exit 1
fi
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "IMPORT_ONLY_OK" ]] || { echo "FAIL: leg 2 program: '$out'"; exit 1; }

echo "OK"
