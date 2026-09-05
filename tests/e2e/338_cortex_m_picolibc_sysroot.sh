#!/usr/bin/env bash
# requires: llvm unix-shell qemu-arm
# A Cortex-M project opts into a C library, and gets the RIGHT multilib.
#
# THE ZERO-LIBC TIER IS THE DEFAULT AND THIS IS THE OPT-IN. Every
# `thumb*-none-eabi*` row carries an empty C-library column, so a project
# targeting one begins with no libc unless it says otherwise. One line says
# otherwise, and `libdir` is what makes that line find anything.
#
# AND THE ASSERTION IS THE FLOAT ABI, NOT THAT IT LINKS.
#
# `libdir` names the sub-directory a multilib C library uses. On riscv
# `<march>/<mabi>` separates every profile because `mabi` there IS the float
# ABI; on ARM `mabi` names the procedure call standard and is `aapcs` for both
# variants, while the float ABI lives in the triple's `eabi`/`eabihf` suffix. So
# `armv7e-m/aapcs` would name ONE directory for two incompatible libraries —
# measured while building `xim:picolibc-arm`, where the soft-float row silently
# received a library carrying `Tag_ABI_HardFP_use`.
#
# A test that only linked would pass either way. This one builds the SOFT-float
# row and asserts the produced image declares no hard-float ABI.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

qemu_arm() {
    local d c
    for d in "${MCPP_HOME:-$HOME/.mcpp}/registry" "$HOME/.xlings"; do
        c=$(ls "$d"/data/xpkgs/xim-x-qemu-arm/*/bin/qemu-system-arm 2>/dev/null | sort -V | tail -1)
        [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return 0; }
    done
    command -v qemu-system-arm 2>/dev/null && return 0
    return 1
}
QEMU="$(qemu_arm)" || { echo "SKIP: qemu-system-arm not installed"; exit 0; }

llvm_tool() {
    local c
    c=$(ls "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-llvm/*/bin/"$1" 2>/dev/null | sort -V | tail -1)
    [ -n "$c" ] && { echo "$c"; return 0; }
    command -v "$1" 2>/dev/null
}
READELF="$(llvm_tool llvm-readelf)" || { echo "SKIP: llvm-readelf not found"; exit 0; }

# The payload has to be present. Installing it here rather than skipping
# would make the test about the install; skipping when it is absent keeps the
# criterion about the ENGINE, which is what this suite tests.
# `xim-x-picolibc-arm` AND NOT `*-x-picolibc-arm`. The engine resolves the
# name `xim:picolibc-arm`, so a copy installed under any other namespace — a
# LOCAL index entry, say — satisfies a glob and not the engine. Measured: with
# the loose pattern this test proceeded against a `local-x-` payload and failed
# on `'stdio.h' file not found`, having asserted nothing about the engine.
sysroot_dir() {
    local d c
    for d in "${MCPP_HOME:-$HOME/.mcpp}/registry" "$HOME/.xlings"; do
        for c in "$d"/data/xpkgs/xim-x-picolibc-arm/*/; do
            [ -d "$c/lib/thumbv7m-none-eabi" ] && { echo "$c"; return 0; }
        done
    done
    return 1
}
SYSROOT="$(sysroot_dir)" || { echo "SKIP: picolibc-arm is not installed"; exit 0; }

mkdir -p "$work/fw/src"
cd "$work/fw"
cat > mcpp.toml <<'TOML'
[package]
name    = "fw"
version = "0.1.0"

[build]
target  = "thumbv7m-none-eabi"
sources = ["src/main.c"]

[targets.fw]
kind = "bin"
main = "src/main.c"

# The opt-in, and the whole of it.
[target.thumbv7m-none-eabi]
sysroot = "xim:picolibc-arm@1.8.12"
TOML

# LOCATION IS A TARGET FACT; SELECTION IS A BOARD FACT. The engine resolved
# WHERE the C library is and which multilib profile applies. WHICH startup
# object, WHICH linker script and WHICH libraries are decisions a board makes,
# and this fixture stands in for a board package.
#
# `target_libc_profile()` IS THE `libdir` COLUMN, AND WITHOUT IT NONE OF THIS
# CAN BE WRITTEN. It is empty when the column is, and the board is then reduced
# to guessing a directory name — which is the thing that goes wrong silently.
cat > build.mcpp <<'BUILD'
import mcpp;
import std;
int main() {
    const std::string root = mcpp::sysroot_dir() ? mcpp::sysroot_dir() : "";
    const std::string prof = mcpp::target_libc_profile()
                           ? mcpp::target_libc_profile() : "";
    if (prof.empty()) {
        std::cerr << "the target row carries no libc profile; a board cannot "
                     "name a file inside the sysroot\n";
        return 1;
    }
    const std::string lib = root + "/lib/" + prof;
    mcpp::link_script((lib + "/picolibc.ld").c_str());
    mcpp::link_search(lib.c_str());
    mcpp::link_lib("crt0-semihost");   // pulled from the archive by ENTRY
    mcpp::link_lib("c");
    mcpp::link_lib("semihost");
    mcpp::link_lib(mcpp::target_builtins_lib() ? mcpp::target_builtins_lib() : "");
    return 0;
}
BUILD

cat > src/main.c <<'C'
#include <stdio.h>
/* `volatile`, so the multiply survives constant folding and picolibc's float
   formatting is really exercised — the path that needs the builtins. */
volatile float fa = 3.0f, fb = 4.0f;
int main(void) { printf("picolibc: %.2f\n", (double)(fa * fb + 1.0f)); return 0; }
C

# Twice, the first allowed to fail: the target world is installed during a build.
"$MCPP" build > /dev/null 2>&1 || true
"$MCPP" build 2>&1 | tail -20 || { echo "FAIL: the opt-in build did not succeed"; exit 1; }

elf=$(find target -type f -name fw | head -1)
[ -n "$elf" ] || { echo "FAIL: no artefact"; exit 1; }

# AN EMPTY IMAGE IS NOT A PASS, AND IT IS WHAT A MISSING BOARD SELECTION
# PRODUCES. Measured while writing this: without the crt0 the link SUCCEEDED and
# mcpp reported `Size fw  text 0  data 0` — a well-formed ELF containing
# nothing. Every check that only looked for "Finished" would have passed.
size=$(wc -c < "$elf")
[ "$size" -gt 4096 ] || { echo "FAIL: the artefact is $size bytes — an empty link"; exit 1; }

# THE ASSERTION THAT CATCHES THE WRONG MULTILIB. A hard-float libc linked
# into a soft-float image leaves `Tag_ABI_HardFP_use` in the attributes.
hard=$("$READELF" -A "$elf" 2>/dev/null | grep -c 'ABI_HardFP_use' || true)
[ "$hard" = "0" ] || {
    echo "FAIL: a soft-float image carries $hard hard-float ABI tags — the wrong multilib was linked"
    exit 1; }
echo "  ok  the soft-float row linked a soft-float C library"

# And it runs: the C library is not merely present, its printf works.
set +e
timeout 30 "$QEMU" -machine mps2-an385 -cpu cortex-m3 -nographic -semihosting \
                   -no-reboot -kernel "$elf" > run.log 2>&1
rc=$?
set -e
cat run.log
grep -q 'picolibc: 13.00' run.log \
    || { echo "FAIL: printf did not produce the expected output"; exit 1; }
[ "$rc" = "0" ] || { echo "FAIL: the image ran but exited $rc"; exit 1; }

echo "PASS: a Cortex-M project opts into picolibc and gets the right multilib"
