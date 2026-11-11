#!/usr/bin/env bash
# requires: llvm aarch64-gnu-sysroot
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1076 -- an aarch64-linux-gnu cross takes its C library from the sysroot
# payload its row names (2026.11.11.1, G1).
#
# `host_can_serve` answered "no payload here produces it" for every arch-cross
# glibc target: llvm alone carries no glibc of another architecture. The
# row's own declaration is the better answer -- the provider that serves the
# identity names a sysroot payload (`xim:aarch64-linux-gnu-sysroot`, the
# package root IS the sysroot: glibc with the kernel headers, libgcc and
# libc++ inside) and states the hosts it is published for, so the gate reads
# the data and no host is named in the engine.
#
# The capability token probes the payload's install directory: the recipe is
# pending in the package index, so a machine without it (CI until the index
# carries it) skips rather than guesses. Where the payload IS installed the
# criteria are: a minimal C program builds for aarch64-linux-gnu, the
# artifact is an aarch64 ELF, and -- with qemu-aarch64 on the machine -- it
# runs and its exit status is passed through.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -25; exit 1; }

# The cap token already probed the registry; re-check so the script also runs
# standalone without run_all.sh.
sysroot=""
for d in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-aarch64-linux-gnu-sysroot/*/; do
    [ -d "$d/lib" ] && sysroot="$d"
done
if [ -z "$sysroot" ]; then
    echo "SKIP: 1076 -- no aarch64-linux-gnu-sysroot payload installed (xim:aarch64-linux-gnu-sysroot)"
    echo "PASS: 1076 (skipped)"
    exit 0
fi

mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'TOML'
[package]
name    = "cross"
version = "0.1.0"

[targets.cross]
kind = "bin"
main = "src/main.c"
TOML
printf '#include <stdio.h>\nint main(void) { printf("cross-ok\\n"); return 42; }\n' > "$t/src/main.c"
cd "$t"
out=$("$MCPP" build --target aarch64-linux-gnu --toolchain "llvm@${LLVM_VERSION}" 2>&1) \
    || fail "aarch64-linux-gnu did not build from the row's sysroot payload" "$out"
exe=$(find target/aarch64-linux-gnu -path '*/bin/cross' -type f | head -1)
[ -n "$exe" ] || fail "no program" "$out"
readelf="$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-readelf"
[ -x "$readelf" ] || fail "no llvm-readelf in the llvm payload ($readelf)"
arch=$("$readelf" -h "$exe" | awk '/Machine:/ {print $2}')
[ "$arch" = "AArch64" ] || fail "the artifact is not an aarch64 ELF (machine: $arch)" "$("$readelf" -h "$exe" | head -20)"
# Where the machine can run it, the program's own exit status is the truth.
if command -v qemu-aarch64 >/dev/null 2>&1; then
    rc=0
    qemu-aarch64 -L "${sysroot%/}" "$exe" > run.log 2>&1 || rc=$?
    [ "$rc" -eq 42 ] || fail "qemu-aarch64 run: expected exit 42, got $rc" run.log
    grep -q '^cross-ok$' run.log || fail "the program's output did not arrive" run.log
fi
echo "PASS: 1076 an aarch64-linux-gnu cross comes from a sysroot payload"
