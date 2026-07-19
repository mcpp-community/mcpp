#!/usr/bin/env bash
# requires: elf gcc
# #249: `[build] include_dirs_after` → -idirafter. A path dependency whose
# include root contains BOTH a file named like a standard header (stdlib.h,
# poisoned) AND a real header (mylib.h) must not break consumers: -idirafter
# dirs are searched AFTER the toolchain's system dirs, so the system stdlib.h
# wins while mylib.h is still found. Control: the same dep exposed via plain
# include_dirs (-I, highest priority) picks the poisoned stdlib.h and the
# build fails. This is the Linux encoding of the macOS case-insensitive
# problem where a tarball root's VERSION file shadows libc++'s <version>.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"

# Dependency — a header-only-ish C++ lib whose include root is poisoned.
"$MCPP" new poisonlib > /dev/null
cd poisonlib
rm -f src/main.cpp
cat > src/lib.cppm <<'EOF'
export module poisonlib.lib;
export int poisonlib_anchor() { return 1; }
EOF
mkdir -p inc
cat > inc/stdlib.h <<'EOF'
#error poisoned
EOF
cat > inc/mylib.h <<'EOF'
#pragma once
inline int mylib_answer() { return 42; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "poisonlib"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[build]
include_dirs_after = ["inc"]
[targets.poisonlib]
kind = "lib"
EOF
cd ..

# Consumer — includes BOTH the standard header the dep poisons and the
# dep's real header.
"$MCPP" new consumer > /dev/null
cd consumer
cat > src/main.cpp <<'EOF'
#include <stdlib.h>
#include <mylib.h>
import std;
int main() {
    std::println("answer = {}", mylib_answer());
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
[dependencies.poisonlib]
path = "../poisonlib"
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "include_dirs_after build failed"; exit 1; }
out=$("$MCPP" run 2>&1)
[[ "$out" == *"answer = 42"* ]] || { echo "dep header not found via -idirafter: $out"; exit 1; }

# The -idirafter flag (not -I) must carry the dep's include root.
ninja_file="$(find target -name build.ninja)"
grep -q -- '-idirafter[^ ]*poisonlib[^ ]*inc' "$ninja_file" || {
    echo "ninja missing -idirafter for dep include root"; exit 1; }

# Control: the same dir via plain include_dirs (-I) precedes system dirs —
# the poisoned stdlib.h is picked and the build MUST fail.
sed -i.bak 's/include_dirs_after = \["inc"\]/include_dirs = ["inc"]/' ../poisonlib/mcpp.toml
rm -f ../poisonlib/mcpp.toml.bak
rm -rf target
"$MCPP" build > poisoned.log 2>&1 && { echo "expected poisoned build to fail"; exit 1; }
grep -q "poisoned" poisoned.log || { cat poisoned.log; echo "wrong failure (not the poison)"; exit 1; }

echo "OK"
