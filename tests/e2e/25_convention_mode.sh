#!/usr/bin/env bash
# 25_convention_mode.sh — verify M5.0 convention-first schema:
#   - 3-line mcpp.toml builds + runs successfully
#   - Inferred banner shown for sources / target
#   - Default include_dirs = ["include"] when ./include/ exists
#   - Library auto-target inferred from .cppm presence (no main.cpp)
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# --- Test 1: minimal binary project (3-line mcpp.toml) ---
mkdir -p "$TMP/tinyapp/src"
cd "$TMP/tinyapp"
cat > mcpp.toml <<'EOF'
[package]
name    = "tinyapp"
version = "0.1.0"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("convention-mode bin OK"); return 0; }
EOF

"$MCPP" build > build.log 2>&1
grep -q 'Inferred sources \[src/\*\*'                build.log || { cat build.log; echo "FAIL: no Inferred sources line"; exit 1; }
grep -q 'Inferred target tinyapp (bin from src/main.cpp)' build.log || { cat build.log; echo "FAIL: no Inferred target line"; exit 1; }
grep -q 'Compiling tinyapp v0.1.0'                    build.log || { cat build.log; echo "FAIL: not compiling"; exit 1; }

triple=$(ls -d target/*/ | head -1)
fp_dir=$(ls "$triple")
out=$("${triple}${fp_dir}/bin/tinyapp")
[[ "$out" == "convention-mode bin OK" ]] || { echo "FAIL: runtime out='$out'"; exit 1; }

# --- Test 2: include/ dir present → default include_dirs picked up ---
mkdir -p "$TMP/inc/src" "$TMP/inc/include/inc"
cd "$TMP/inc"
cat > mcpp.toml <<'EOF'
[package]
name    = "inc"
version = "0.1.0"
EOF
cat > include/inc/api.hpp <<'EOF'
#pragma once
inline int answer() { return 42; }
EOF
cat > src/main.cpp <<'EOF'
#include <inc/api.hpp>
import std;
int main() { std::println("answer = {}", answer()); return 0; }
EOF
"$MCPP" build > build.log 2>&1
grep -q 'Inferred include_dirs \[include\]' build.log || {
    cat build.log; echo "FAIL: include/ not auto-picked"; exit 1; }
triple=$(ls -d target/*/ | head -1)
fp_dir=$(ls "$triple")
out=$("${triple}${fp_dir}/bin/inc")
[[ "$out" == "answer = 42" ]] || { echo "FAIL: include resolution: $out"; exit 1; }

# --- Test 3: library project (no main.cpp, has .cppm) → auto kind=lib ---
mkdir -p "$TMP/mylib/src"
cd "$TMP/mylib"
cat > mcpp.toml <<'EOF'
[package]
name    = "mylib"
version = "0.1.0"
EOF
cat > src/mylib.cppm <<'EOF'
export module mylib;
import std;
export auto greet() -> void { std::println("from auto-lib"); }
EOF
"$MCPP" build > build.log 2>&1
grep -q 'Inferred target mylib (lib from .cppm in src/)' build.log || {
    cat build.log; echo "FAIL: lib auto-target not inferred"; exit 1; }
grep -q 'Finished' build.log || { cat build.log; echo "FAIL: lib didn't finish"; exit 1; }

# --- Test 4: backward-compat — old [language]/[modules] schema still works ---
mkdir -p "$TMP/legacy/src"
cd "$TMP/legacy"
cat > mcpp.toml <<'EOF'
[package]
name    = "legacy"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cpp"]
[targets.legacy]
kind = "bin"
main = "src/main.cpp"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("legacy schema OK"); return 0; }
EOF
"$MCPP" build > build.log 2>&1
# Should NOT see Inferred lines (everything explicit).
if grep -q 'Inferred' build.log; then
    cat build.log; echo "FAIL: legacy schema fired Inferred banner unexpectedly"; exit 1
fi
triple=$(ls -d target/*/ | head -1)
fp_dir=$(ls "$triple")
out=$("${triple}${fp_dir}/bin/legacy")
[[ "$out" == "legacy schema OK" ]] || { echo "FAIL: legacy runtime: $out"; exit 1; }

echo "OK"
