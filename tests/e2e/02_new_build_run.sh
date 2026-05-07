#!/usr/bin/env bash
# Single-module hello world: mcpp new → build → run
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new hello
cd hello

# Validate generated structure
[[ -f mcpp.toml ]]      || { echo "missing mcpp.toml"; exit 1; }
[[ -f src/main.cpp ]]   || { echo "missing src/main.cpp"; exit 1; }
[[ -f .gitignore ]]     || { echo "missing .gitignore"; exit 1; }
grep -q "import std"           src/main.cpp || { echo "main.cpp missing 'import std'"; exit 1; }
grep -q "std::println"          src/main.cpp || { echo "main.cpp missing 'std::println'"; exit 1; }

# Build
"$MCPP" build > build.log 2>&1
[[ -d target ]] || { cat build.log; echo "no target/ dir"; exit 1; }
binary="$(find target -name hello -type f | head -1)"
[[ -n "$binary" ]] || { echo "binary not produced"; exit 1; }
[[ -x "$binary" ]] || { echo "binary not executable"; exit 1; }

# Run via mcpp
out=$("$MCPP" run 2>&1)
[[ "$out" == *"Hello from hello"* ]] || { echo "unexpected run output: $out"; exit 1; }

# Run with passthrough args
out=$("$MCPP" run -- foo bar 2>&1)
[[ "$out" == *"foo"* && "$out" == *"bar"* ]] || { echo "args passthrough failed: $out"; exit 1; }

echo "OK"
