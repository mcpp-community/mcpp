#!/usr/bin/env bash
# Project pins [toolchain] → mcpp resolves to xpkg absolute path.
#
# This test verifies the resolve_xpkg_path branch is exercised when a
# pinned toolchain spec maps to an existing xpkg payload. We seed the
# sandbox xpkgs from the user's system xlings cache via symlink to
# avoid network-bound install delay. We DO NOT run the full compile
# (that would require glibc/sysroot env activation, which is xlings'
# subos shim concern, not mcpp's toolchain-resolve feature).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

SANDBOX_HOME="$TMP/mcpp-home"
mkdir -p "$SANDBOX_HOME/registry/data/xpkgs"
if [[ -d "$HOME/.xlings/data/xpkgs/xim-x-gcc/16.1.0" ]]; then
    ln -s "$HOME/.xlings/data/xpkgs/xim-x-gcc" \
          "$SANDBOX_HOME/registry/data/xpkgs/xim-x-gcc"
else
    echo "SKIP: system xlings has no gcc 16.1.0"
    echo "OK"
    exit 0
fi

export MCPP_HOME="$SANDBOX_HOME"

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

cat > mcpp.toml <<'EOF'
[package]
name        = "myapp"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[toolchain]
linux = "gcc@16.1.0"
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.myapp]
kind = "bin"
main = "src/main.cpp"
EOF

# We tolerate compile failure (env-activation issue) — only assert that the
# resolve stage produced the expected output.
out=$("$MCPP" build 2>&1 || true)

# 1. "Resolving" + "toolchain" verb appeared
echo "$out" | grep -q 'Resolving' || { echo "missing Resolving: $out"; exit 1; }
echo "$out" | grep -q 'toolchain'  || { echo "missing toolchain: $out"; exit 1; }

# 2. Resolved line points at the xpkg payload under our sandbox.
#    The display path is now shortened — paths under MCPP_HOME
#    render as `@mcpp/...` — so we match the shortened form
#    rather than the absolute one. The trailing path components are
#    what actually prove the toolchain came from mcpp's sandbox.
echo "$out" | grep -q 'Resolved.*gcc@16.1.0' || {
    echo "missing Resolved gcc@16.1.0: $out"; exit 1; }
echo "$out" | grep -q '@mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++' || {
    echo "Resolved path doesn't reference the mcpp sandbox xpkg: $out"; exit 1; }

echo "OK"
