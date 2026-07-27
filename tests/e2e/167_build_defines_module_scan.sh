#!/usr/bin/env bash
# requires:
# [build].defines must reach the P1689 module scan AND the compile command
# for every TU (including module interfaces). Previously it was parsed but
# silently dropped, causing plan-vs-scan divergence for conditional imports.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p src

cat > src/lib.cppm <<'EOF'
#ifndef TEST_USE_MODULES
module;
#include <print>
#endif

export module lib;

#ifdef TEST_USE_MODULES
import std;
#endif

export void printHello(){
    std::println("hello from lib");
}
EOF

cat > src/main.cpp <<'EOF'
#ifdef TEST_USE_MODULES
import std;
import lib;
#else
#include <print>
#endif

int main() {
    std::println("hello from main");
    printHello();
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name        = "hello"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"

[build]
defines = ["TEST_USE_MODULES"]

[scan_overrides."src/lib.cppm"]
provides = ["lib"]
imports = ["std"]

[scan_overrides."src/main.cpp"]
imports = ["std", "lib"]
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
out=$("$MCPP" run 2>&1 | tail -2 | tr '\n' ' ')
[[ "$out" == *"hello from main"*"hello from lib"* ]] || {
    echo "unexpected output: $out"; exit 1; }

echo "OK"
