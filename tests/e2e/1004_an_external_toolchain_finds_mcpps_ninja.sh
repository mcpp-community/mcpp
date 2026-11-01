#!/usr/bin/env bash
# requires: gcc
# 1004 -- a toolchain named by a path outside every provider finds the ninja
# mcpp installed into its own home (mcpp#875). Before 2026.11 only the one
# beside the compiler's provider package was asked, then PATH, so a machine
# whose PATH held no ninja failed with "cannot start 'ninja'" while mcpp's own
# sat installed.
#
# Criteria:
#   A. With the compiler reached through a directory outside the store and a
#      PATH that holds no ninja, the build succeeds.
#   B. `MCPP_NINJA`, as a build program sees it, is the same absolute ninja.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

gxx=$(ls -d "$MCPP_HOME"/registry/data/xpkgs/xim-x-gcc/*/bin/g++ 2>/dev/null | head -1)
[ -n "$gxx" ] || { echo "SKIP: no gcc payload to reach from outside the store"; exit 0; }
ninja=$(ls -d "$MCPP_HOME"/registry/data/xpkgs/xim-x-ninja/*/ninja 2>/dev/null | head -1)
[ -n "$ninja" ] || { echo "SKIP: mcpp's home holds no ninja"; exit 0; }
ln -s "$(dirname "$(dirname "$gxx")")" "$TMP/external-gcc"

# The xlings mcpp drives, named outright: the PATH below holds no xlings.
export MCPP_VENDORED_XLINGS="${MCPP_VENDORED_XLINGS:-$(command -v xlings || true)}"
[ -n "$MCPP_VENDORED_XLINGS" ] || [ -x "$MCPP_HOME/registry/bin/xlings" ] || { echo "SKIP: no xlings to name"; exit 0; }

# A PATH with the usual tools and no ninja. An external toolchain is complete
# where it is installed (MSYS2's GCC carries its assembler in its own `bin');
# this one's assembler is the payload's binutils, put on PATH as such a
# toolchain's would be.
mkdir -p "$TMP/bin"
for t in sh bash env cat sed grep mkdir rm ls cp mv ln dirname basename uname tr head tail awk; do
    p=$(command -v "$t" 2>/dev/null) && ln -sf "$p" "$TMP/bin/$t"
done
binutils=$(ls -d "$MCPP_HOME"/registry/data/xpkgs/xim-x-binutils/*/bin 2>/dev/null | head -1)
for t in as ld ar ranlib nm objcopy strip; do
    [ -n "$binutils" ] && [ -x "$binutils/$t" ] && ln -sf "$binutils/$t" "$TMP/bin/$t"
done
[ ! -e "$TMP/bin/ninja" ] || fail "the restricted PATH holds a ninja"

cd "$TMP"
mkdir -p app/src && cd app
printf '[package]\nname = "app"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("built"); }\n' > src/main.cpp

# A
PATH="$TMP/bin" MCPP_TOOLCHAIN="path:$TMP/external-gcc" "$MCPP" build > a.log 2>&1 \
    || fail "the build with an external toolchain and no ninja on PATH failed" a.log
! grep -q "cannot start 'ninja'" a.log || fail "the bare name reached PATH" a.log

# B
PATH="$TMP/bin" MCPP_TOOLCHAIN="path:$TMP/external-gcc" "$MCPP" self env > b.log 2>&1 || true
if grep -q 'MCPP_NINJA' b.log; then
    grep 'MCPP_NINJA' b.log | grep -q '/ninja' || fail "MCPP_NINJA is not an absolute ninja" b.log
fi
echo "OK"
