#!/usr/bin/env bash
# requires: llvm unix-shell
# `aarch64-none-elf`: the second bare-metal architecture, on the zero-libc tier.
#
# ⚠️ WHY A TARGET ROW NEEDS A TEST AT ALL
#
# A row in the target table is four strings, and three of them can be wrong in
# ways that still produce a build. A wrong `-mabi` is rejected by the driver,
# which is the loud case; a wrong code model links and faults only when the
# image is placed somewhere the model cannot address; a sysroot column filled in
# "for symmetry" resolves a payload that does not exist and silently adds no
# paths, so the failure surfaces much later as a missing header inside somebody
# else's package.
#
# So this asserts what the row PRODUCES rather than what it says: an image whose
# sections are where the linker script asked, with no undefined symbols, from a
# project that names no C library.
#
# No emulator is needed. Whether the image boots is asserted by openarch's own
# CI, which runs one probe on this machine and on riscv64; what belongs here is
# that the engine can produce it at all.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new a64probe > /dev/null
cd a64probe
rm -f tests/*.cpp 2>/dev/null || true

# ⚠️ NO `[target.*]` SECTION, AND THAT IS THE ASSERTION. The zero-libc tier is
# this row's own property, not something the project asked for — which is the
# difference from `riscv64-none-elf`, where declining the C library takes an
# explicit `sysroot = ""`.
cat > mcpp.toml <<'EOF'
[package]
name    = "a64probe"
version = "0.1.0"

[build]
target = "aarch64-none-elf"
EOF

cat > src/main.cpp <<'EOF'
namespace {
volatile unsigned int* const kUart = reinterpret_cast<unsigned int*>(0x09000000);
}
extern "C" void kmain() {
    for (const char* p = "a64 ok\n"; *p; ++p) *kUart = static_cast<unsigned int>(*p);
    for (;;) { asm volatile("wfi"); }
}
asm(".section .text.entry,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "  ldr x30, =__stack_top\n"
    "  mov sp, x30\n"
    "  bl kmain\n"
    "1: b 1b\n");
EOF

cat > link.ld <<'EOF'
ENTRY(_start)
SECTIONS {
    . = 0x40000000;
    .text   : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    . = ALIGN(16); . = . + 0x4000; __stack_top = .;
}
EOF

cat > build.mcpp <<'EOF'
import mcpp;
int main() { mcpp::link_script("link.ld"); return 0; }
EOF

# ── 1. The target builds with no C library anywhere in the project ──────────
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "aarch64-none-elf did not build"; exit 1; }

ELF=$(find target/aarch64-none-elf -type f -name a64probe | head -1)
[ -n "$ELF" ] || { cat build.log; echo "no image was produced"; exit 1; }

READELF=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm" -name llvm-readelf 2>/dev/null | head -1)
NM=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm" -name llvm-nm 2>/dev/null | head -1)
[ -x "$READELF" ] && [ -x "$NM" ] || { echo "the LLVM payload has no readelf/nm to check with"; exit 1; }

# ── 2. It is aarch64, and it starts where the script says ───────────────────
"$READELF" -h "$ELF" | grep -q "AArch64" || {
    "$READELF" -h "$ELF" | head -12; echo "the image is not aarch64"; exit 1; }
"$READELF" -S "$ELF" | grep -E '\.text' | grep -q '0000000040000000' || {
    "$READELF" -S "$ELF" | head -8
    echo "the linker script's load address did not reach the image"; exit 1; }

# ── 3. Nothing is left undefined ────────────────────────────────────────────
# ⚠️ The load-bearing one. A row whose sysroot column named a payload that does
# not exist would add no paths and produce exactly this symptom later, inside
# whichever package first included a C header — so the absence of a C library is
# asserted as an absence of undefined symbols rather than as a missing key.
UNDEF=$("$NM" -u "$ELF" | wc -l)
[ "$UNDEF" -eq 0 ] || {
    "$NM" -u "$ELF"; echo "the image expects $UNDEF symbols nothing provides"; exit 1; }

# ── 4. The ISA profile is the measured one, not the analogous one ───────────
# `-mabi=lp64` is what the RISC-V rows' spelling suggests and what clang
# rejects with `unknown target ABI`. Asserting the flag rather than the outcome,
# because a build that had succeeded with the wrong ABI would be the more
# dangerous result.
rm -rf target
"$MCPP" build --verbose > verbose.log 2>&1 || true
grep -q -- "-mabi=aapcs" verbose.log || {
    grep -o -- '-mabi=[a-z0-9]*' verbose.log | sort -u
    echo "the aarch64 ABI flag is not aapcs"; exit 1; }
if grep -q -- "-mabi=lp64" verbose.log; then
    grep -o -- '-mabi=[a-z0-9]*' verbose.log | sort -u
    echo "an ABI clang rejects reached the command line"; exit 1
fi

echo "PASS: aarch64-none-elf builds on the zero-libc tier with the measured ISA profile"
