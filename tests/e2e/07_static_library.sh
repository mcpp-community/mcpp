#!/usr/bin/env bash
# Static library: kind = "lib"  → libNAME.a via `ar rcs`
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new mathlib > /dev/null
cd mathlib

cat > src/math.cppm <<'EOF'
export module mathlib;
import std;
export int add(int a, int b) { return a + b; }
export int mul(int a, int b) { return a * b; }
EOF
rm src/main.cpp

cat > mcpp.toml <<'EOF'
[package]
name        = "mathlib"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.mathlib]
kind = "lib"
EOF

"$MCPP" build > build.log 2>&1
archive="$(find target -name 'libmathlib.a' | head -1)"
[[ -n "$archive" ]]   || { cat build.log; echo "no .a produced"; exit 1; }
[[ -s "$archive" ]]   || { echo "empty .a"; exit 1; }
# Use the sandbox `ar` directly so this test doesn't depend on the host's
# PATH having an active binutils pin (xlings shim with no version on a
# fresh CI runner returns "no version set for ar").
SANDBOX_AR="$(find "${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs/xim-x-binutils" \
    -path '*/bin/ar' -type f | head -1)"
AR="${SANDBOX_AR:-ar}"
"$AR" t "$archive" | grep -q '\.o$' || { echo ".a contains no objects"; exit 1; }

# build.ninja must use cxx_archive rule for the lib target
ninja_file="$(find target -name build.ninja)"
grep -q 'bin/libmathlib.a : cxx_archive' "$ninja_file" || {
    echo "build.ninja did not use cxx_archive for the lib target"; exit 1; }

echo "OK"
