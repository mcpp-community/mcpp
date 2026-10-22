#!/usr/bin/env bash
# requires: gcc llvm elf unix-shell
# `glibc@system`: the host's own root as the target base (design 2026-10-23
# D11, S10) -- for a build that packages for the distribution it runs on, or
# links the host's libraries.
#
# THE TWO DRIVERS DIFFER, AND THAT IS THE POINT OF RUNNING BOTH. A gcc built
# without Debian multiarch support finds no `crt1.o` in `/usr/lib/<tuple>`
# (`--sysroot=/` fails with `cannot find crti.o`, measured as scenario J);
# clang knows the layout itself (scenario K). The engine measures which driver
# needs the multiarch directories and supplies them to that one only.
set -e

MCPP="${MCPP:-mcpp}"
source "$(dirname "$0")/_toolchain_env.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

tuple="$(uname -m)-linux-gnu"
if [[ ! -e "/usr/lib/$tuple/crt1.o" ]]; then
    echo "SKIP: this host is not a Debian multiarch layout (/usr/lib/$tuple/crt1.o)"; exit 0
fi
host_glibc=$(getconf GNU_LIBC_VERSION | awk '{print $2}')

mkdir -p "$work/app/src"
cd "$work/app"
cat > src/main.cpp <<'CPP'
#include <gnu/libc-version.h>
import std;
int main() { std::println("built {}.{} ran {}", __GLIBC__, __GLIBC_MINOR__, gnu_get_libc_version()); }
CPP
for tc in "gcc@${GCC_VERSION}" "llvm@${LLVM_VERSION}"; do
    cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"
[toolchain]
default = "$tc"
[target.$(uname -m)-linux-gnu]
sysroot = "glibc@system"
TOML
    rm -rf target
    out="$("$MCPP" run 2>&1)" || fail "$tc: glibc@system failed"
    grep -q "Using sysroot glibc .* ← glibc@system  \[host · mcpp.toml:" <<<"$out" \
        || fail "$tc: no Using line for the host's base"
    grep -q "ran $host_glibc" <<<"$out" || fail "$tc: the program did not run on the host's glibc"
    a=$(ls target/*/*/bin/app | head -1)
    readelf -d "$a" | grep -q "subos" && fail "$tc: the SubOS farm reached a host-based artifact"
    readelf -l "$a" | grep -q "Requesting program interpreter: /lib" || fail "$tc: PT_INTERP is not the host's loader"
    why="$("$MCPP" why sysroot 2>&1)"
    if [[ "$tc" == gcc@* ]]; then
        grep -q "multiarch=$tuple" <<<"$why" || fail "gcc: the multiarch directories were not measured"
    else
        grep -q "multiarch=" <<<"$why" && fail "llvm: multiarch was supplied to a driver that knows it"
    fi
    echo "  ok: $tc builds and runs on the host's own base"
done
echo "PASS: glibc@system is the host's base, multiarch supplied only where measured"
