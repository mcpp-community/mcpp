#!/usr/bin/env bash
# 179_spaced_paths.sh — a project whose paths contain spaces still builds
#
# #331: two independent places assumed no path ever contains a space. On
# Windows that means assuming nobody installs under `C:\Program Files` and no
# user account name has a space in it — both routinely false. The failures are
# not Windows-specific though (a Linux `/home/my dir/inc` splits identically),
# so this runs everywhere: same bug, much faster feedback.
set -e

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Every path below the project root carries a space, including the project
# directory itself.
mkdir -p "$TMP/my work dir"
cd "$TMP/my work dir"
"$MCPP" new "spaced_proj" >/dev/null 2>&1
cd spaced_proj

# An include directory with a space, holding a header reachable ONLY through
# it — so a split include flag fails to compile rather than silently passing.
mkdir -p "vendor inc/deep dir"
cat > "vendor inc/deep dir/spaced_header.hpp" <<'EOF'
#pragma once
inline int spaced_header_value() { return 4242; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "spaced_proj"
version = "0.1.0"

[build]
include_dirs = ["vendor inc/deep dir"]
EOF

# A build.mcpp exercises the other half: mcpp compiles and execs it, so its
# own argv carries the (spaced) payload path of the host compiler.
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    std::puts("mcpp:cfg=SPACED_BUILD_PROGRAM_RAN");
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
#include <spaced_header.hpp>
import std;
int main() {
#ifndef SPACED_BUILD_PROGRAM_RAN
    std::println("build.mcpp directive missing");
    return 1;
#endif
    if (spaced_header_value() != 4242) {
        std::println("wrong header value");
        return 1;
    }
    std::println("spaced-ok");
    return 0;
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: build in a spaced path: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run: $run_out"; exit 1; }
[[ "$run_out" == *"spaced-ok"* ]] || { echo "FAIL: run output: $run_out"; exit 1; }

# The include flag must reach the compiler as ONE shell word. Assert on the
# generated ninja file rather than on the build succeeding by luck: a split
# flag can still compile if the header happens to be findable another way.
ninja_file=$(find target -name build.ninja | head -1)
[[ -n "$ninja_file" ]] || { echo "FAIL: no build.ninja found"; exit 1; }
grep -q "vendor" "$ninja_file" || {
    echo "FAIL: include dir absent from build.ninja"; exit 1; }
# TWO escaping layers, in order, and the test has to know both: ninja escapes
# the space as `$ ` (so ninja itself does not treat it as a separator), and
# the shell quoting wraps the whole token (so what ninja hands to sh stays one
# word). The old code had only the first, which is exactly why the path
# survived ninja and then split in the shell.
# The quote character is the host shell's — POSIX sh single, cmd.exe double —
# so accept either rather than pinning whichever platform this happens to run
# on. (An earlier version hardcoded `'` and failed on Windows for a difference
# that was correct.)
grep -qE "['\"]-I[^'\"]*vendor\\\$ inc" "$ninja_file" || {
    echo "FAIL: include dir not shell-quoted with its prefix:"
    grep -oE "[^ ]*vendor[^ ]*( inc[^ ]*)?" "$ninja_file" | head -3
    exit 1; }

echo "PASS: spaced paths — include dirs, project root and build.mcpp"
