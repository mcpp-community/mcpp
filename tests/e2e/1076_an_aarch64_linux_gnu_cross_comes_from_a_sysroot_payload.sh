#!/usr/bin/env bash
# requires: llvm elf x86_64-host
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1076 -- the cross arrangement of aarch64-linux-gnu: its C library, C++
# library and compiler run-time come from a base package, its artifacts name
# the row's loader, and they run through the provider's runner (SPEC-016 §3,
# §6; design 2026-11-12 C2, C3, C5).
#
# The row has two provider entries: `glibc`, the host's payload on an aarch64
# host, and `glibc-sysroot`, a base package on linux-x86_64 at a priority
# below the musl entries. The second is what this script holds:
#
#   1. `aarch64-linux` is answered as before the entry existed (musl); the
#      whole identity reaches the cross entry.
#   2. A C program, a C++ program over the headers and a program that imports
#      std each build, dynamically; the last one statically as well.
#   3. Each dynamic artifact is an aarch64 ELF whose interpreter is the row's
#      `loader`, with no RUNPATH and no path of this machine in it.
#   4. `mcpp run` starts each through the provider's runner, which this run
#      installs, and the program's exit status and output arrive.
#
# The entry's hosts are linux-x86_64, which the capability line states: on
# another host the script is excluded, and the run reports it as such.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -25; exit 1; }

readelf="$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-readelf"

T=aarch64-linux-gnu
LOADER=/lib/ld-linux-aarch64.so.1

project() {   # <dir> <main file> <source text>
    mkdir -p "$t/$1/src"
    printf '[package]\nname    = "cross"\nversion = "0.1.0"\n\n[targets.cross]\nkind = "bin"\nmain = "src/%s"\n' "$2" \
        > "$t/$1/mcpp.toml"
    printf '%b' "$3" > "$t/$1/src/$2"
}
project c   main.c   '#include <stdio.h>\nint main(void) { printf("cross-c\\n"); return 42; }\n'
project cxx main.cpp '#include <print>\nint main() { std::println("cross-cxx"); return 43; }\n'
project std main.cpp 'import std;\nint main() { std::println("cross-std"); return 44; }\n'

# 1. The request `aarch64-linux` keeps its answer.
cd "$t/c"
out=$("$MCPP" why target --target aarch64-linux 2>&1) || fail "why target refused" "$out"
grep -q "aarch64-linux-musl" <<<"$out" || fail "aarch64-linux is no longer answered by the musl identity" "$out"

check() {   # <dir> <output word> <exit status>
    cd "$t/$1"
    out=$("$MCPP" build --target "$T" 2>&1) || fail "$1: $T did not build over the base package" "$out"
    grep -q "glibc-sysroot\|aarch64-linux-gnu-sysroot" <<<"$out" || fail "$1: the base package is not what served it" "$out"
    exe=$(find "target/$T" -path '*/bin/cross' -type f | head -1)
    [ -n "$exe" ] || fail "$1: no program" "$out"
    [ -x "$readelf" ] || fail "no llvm-readelf in the llvm payload ($readelf)"
    hdr=$("$readelf" -h "$exe")
    grep -q "Machine:.*AArch64" <<<"$hdr" || fail "$1: the artifact is not an aarch64 ELF" "$hdr"
    interp=$("$readelf" -l "$exe" | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p')
    [ "$interp" = "$LOADER" ] || fail "$1: the interpreter is '$interp', the row's loader is $LOADER"
    dyn=$("$readelf" -d "$exe")
    if grep -qiE "RPATH|RUNPATH" <<<"$dyn"; then fail "$1: the artifact carries a run-time path of this machine" "$dyn"; fi
    if grep -aq "${MCPP_HOME:-$HOME/.mcpp}" "$exe"; then fail "$1: a path of this machine's store is in the artifact"; fi
    rc=0
    run=$("$MCPP" run --target "$T" 2>&1) || rc=$?
    [ "$rc" -eq "$3" ] || fail "$1: mcpp run exited $rc, the program returns $3" "$run"
    grep -q "^$2\$" <<<"$run" || fail "$1: the program's output did not arrive" "$run"
    grep -q "Running .*qemu-aarch64" <<<"$run" || fail "$1: the run did not go through the provider's runner" "$run"
}
check c   cross-c   42
check cxx cross-cxx 43
check std cross-std 44

# 2. Statically: no interpreter, and the same exit status through the runner.
cd "$t/std"
printf '\n[target.%s]\nlinkage = "static"\n' "$T" >> mcpp.toml
out=$("$MCPP" build --target "$T" 2>&1) || fail "the static import-std program did not build" "$out"
exe=$(find "target/$T" -path '*/bin/cross' -type f -newer mcpp.toml | head -1)
[ -n "$exe" ] || fail "no static program" "$out"
if "$readelf" -l "$exe" | grep -q "program interpreter"; then fail "a static program names an interpreter"; fi
rc=0
run=$("$MCPP" run --target "$T" 2>&1) || rc=$?
[ "$rc" -eq 44 ] || fail "static: mcpp run exited $rc, the program returns 44" "$run"

echo "PASS: 1076 an aarch64-linux-gnu cross comes from a sysroot payload"
