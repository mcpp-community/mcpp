#!/usr/bin/env bash
# requires: windows msvc
# `msvc@system` IS LOCATED FOR A BUILD PROGRAM, NEVER INSTALLED (2026.11.7.1).
# On a Windows machine whose default toolchain is `msvc@system`, a project with
# a build program built for a bare cross target resolved the build program's
# host toolchain through the package index and looked for an `msvc@system`
# package there (openkal's 32-bit layout probe, design 2026-11-07 §4.1.3). The
# build program is now compiled by the located Visual Studio, as a native build
# of the same machine is, and its directive reaches the cross build.
set -e

CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
    ORIG_DEFAULT=$(sed -n '/^\[toolchain\]/,/^\[/p' "$CONF" \
        | grep -E '^default[[:space:]]*=' | head -1 | cut -d'"' -f2 || true)
fi
TMP=$(mktemp -d)
restore() {
    if [[ -n "$ORIG_DEFAULT" ]]; then
        "$MCPP" toolchain default "$ORIG_DEFAULT" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap restore EXIT

"$MCPP" toolchain default msvc >/dev/null 2>&1 \
    || { echo "FAIL: msvc@system not selectable"; exit 1; }

mkdir -p "$TMP/probe/src" "$TMP/probe/inc"
cd "$TMP/probe"
cat > mcpp.toml <<'EOF2'
[package]
name    = "layout"
version = "0.1.0"
EOF2
cat > build.mcpp <<'EOF2'
#include <cstdio>
int main() {
    std::puts("mcpp:include-dir=inc");
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF2
cat > inc/layout.h <<'EOF2'
struct pair_layout { unsigned size; unsigned long long stamp; };
EOF2
cat > src/main.cpp <<'EOF2'
#include "layout.h"
static_assert(__builtin_offsetof(pair_layout, stamp) == 8);
extern "C" void _start() { for (;;) {} }
EOF2

out=$("$MCPP" build --target riscv64-none-elf 2>&1) \
    || { echo "FAIL: the cross build with a build program failed:"; echo "$out"; exit 1; }
grep -q "host toolchain for build.mcpp: msvc@system" <<< "$out" \
    || { echo "FAIL: the build program was not compiled by the located msvc@system:"; echo "$out"; exit 1; }
if grep -qi "xim:msvc@system\|msvc@system.*not found in the" <<< "$out"; then
    echo "FAIL: msvc@system was looked up as a package:"; echo "$out"; exit 1
fi
echo "OK: msvc@system compiled the build program of a riscv64-none-elf build"
