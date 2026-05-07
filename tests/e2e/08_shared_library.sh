#!/usr/bin/env bash
# Shared library: kind = "shared" → libNAME.so via -shared -fPIC
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
kind = "shared"
EOF

"$MCPP" build > build.log 2>&1
so="$(find target -name 'libmathlib.so' | head -1)"
[[ -n "$so" ]] || { cat build.log; echo "no .so produced"; exit 1; }

# Verify it's a real ELF shared object
file "$so" | grep -q 'ELF.*shared object' || { echo "not a valid .so"; exit 1; }

# build.ninja must use cxx_shared rule
ninja_file="$(find target -name build.ninja)"
grep -q 'bin/libmathlib.so : cxx_shared' "$ninja_file" || {
    echo "build.ninja did not use cxx_shared for the shared lib target"; exit 1; }

# cxxflags must include -fPIC when shared lib is present
grep -E '^cxxflags.*-fPIC' "$ninja_file" || {
    echo "cxxflags did not include -fPIC for shared-lib build"; exit 1; }

echo "OK"
