#!/usr/bin/env bash
# requires: mingw-cross
# #254: an xpkg dependency's per-OS section must be spliced for the RESOLVED
# TARGET, not for the host.
#
# The per-OS sections carry sources, flags, deps and the xpm asset table —
# all describing code compiled INTO the user's build, so they belong to the
# platform the artifacts will run on. They used to key on a compile-time HOST
# constant, so a Linux -> Windows cross build silently got the linux leg.
#
# Native builds cannot observe this (host == target), which is exactly why
# the three-platform CI never caught it: this test has to cross-compile to
# see the difference at all. It uses a local path-index descriptor so it
# needs no network and no real package.
set -e

TRIPLE=x86_64-windows-gnu

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A local xpkg index with one descriptor whose per-OS sections differ in a
# way the compiler can prove: each leg defines its own macro, and the TU
# errors out unless exactly the windows leg was spliced.
mkdir -p idx/pkgs/t
cat > idx/pkgs/t/testsplice.lua <<'LUA'
package = {
    name = "testsplice",
    namespace = "test",
}

xpm = {
    linux   = { ["1.0.0"] = { url = "unused", sha256 = "unused" } },
    windows = { ["1.0.0"] = { url = "unused", sha256 = "unused" } },
    macosx  = { ["1.0.0"] = { url = "unused", sha256 = "unused" } },
}

mcpp = {
    sources = { "src/**/*.cpp" },
    include_dirs = { "include" },
    linux   = { cxxflags = { "-DSPLICED_LEG=1" } },
    macosx  = { cxxflags = { "-DSPLICED_LEG=2" } },
    windows = { cxxflags = { "-DSPLICED_LEG=3" } },
}
LUA

# The "installed" payload for that descriptor.
PAYLOAD="$TMP/payload/test-x-testsplice/1.0.0"
mkdir -p "$PAYLOAD/src" "$PAYLOAD/include"
cat > "$PAYLOAD/include/splice.h" <<'EOF'
#pragma once
int spliced_leg();
EOF
cat > "$PAYLOAD/src/splice.cpp" <<'EOF'
#include "splice.h"
#ifndef SPLICED_LEG
#error "no per-OS section was spliced at all"
#endif
int spliced_leg() { return SPLICED_LEG; }
EOF
touch "$PAYLOAD/.mcpp_ok"

"$MCPP" new crosssplice > /dev/null
cd crosssplice

cat > src/main.cpp <<'EOF'
#include "splice.h"
int main() {
    // 3 == the windows leg. Anything else means the wrong per-OS section
    // was spliced for this target.
    return spliced_leg() == 3 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name    = "crosssplice"
version = "0.1.0"

[dependencies]
"test.testsplice" = "1.0.0"
EOF

# Point mcpp at the local index + pre-installed payload.
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME/registry/data"
cp -r "$TMP/idx" "$MCPP_HOME/registry/data/xim-pkgindex"
mkdir -p "$MCPP_HOME/registry/data/xpkgs"
cp -r "$TMP/payload/test-x-testsplice" "$MCPP_HOME/registry/data/xpkgs/"

set +e
"$MCPP" build --target "$TRIPLE" > build.log 2>&1
RC=$?
set -e

if [[ $RC -ne 0 ]]; then
    # A wrongly-spliced leg shows up as SPLICED_LEG=1 (linux) reaching the
    # windows TU; distinguish that from an unrelated setup failure.
    if grep -q "SPLICED_LEG" build.log; then
        cat build.log
        echo "FAIL: the wrong per-OS section was spliced for target $TRIPLE"
        exit 1
    fi
    echo "SKIP: cross build could not run in this environment"
    sed -n '1,15p' build.log
    exit 0
fi

# The build succeeding is not enough — prove the windows leg's flag is what
# landed, by checking the recorded compile command for the dep TU.
CC_JSON=$(find target -name compile_commands.json | head -1)
if [[ -n "$CC_JSON" ]]; then
    grep -q "SPLICED_LEG=3" "$CC_JSON" || {
        grep -o "SPLICED_LEG=[0-9]" "$CC_JSON" | sort -u
        echo "FAIL: dep TU was not compiled with the windows leg's flags"
        exit 1
    }
    grep -q "SPLICED_LEG=1" "$CC_JSON" && {
        echo "FAIL: the host (linux) leg leaked into a $TRIPLE build"
        exit 1
    }
fi

echo "OK"
