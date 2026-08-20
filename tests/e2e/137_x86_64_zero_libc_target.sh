#!/usr/bin/env bash
# requires: llvm unix-shell
# `x86_64-none-elf`: the third bare-metal architecture, and the first whose
# LINK the compiler driver refuses to perform.
#
# ⚠️ WHAT THIS ROW ADDED THAT THE OTHER TWO DID NOT NEED
#
# `riscv64-none-elf` and `aarch64-none-elf` are four strings in a table and
# nothing else: clang has a BareMetal toolchain for both, so it drives their
# links itself and reaches `ld.lld`. It has none for x86_64, so that triple
# falls through to the generic GCC toolchain — whose linker is the HOST'S `g++`:
#
#     g++: error: unrecognized command-line option
#          '-fuse-ld=/…/llvm/22.1.8/bin/ld.lld'
#
# Measured for every spelling of a bare x86_64 triple, and unfixable by any flag
# (`-fuse-ld=lld`, `--ld-path=`, `--gcc-toolchain=`, `-B` were each tried). Only
# putting `linux` in the OS position makes clang link directly, and that brings
# eight host `-L` paths onto a bare-metal link.
#
# So this row carries an `lldEmulation` and the engine links it with `ld.lld`
# itself. Three of the checks below exist for that path specifically: that the
# host toolchain is not consulted, that driver-only flags do not survive onto a
# linker's command line, and that the map is still produced.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new x86probe > /dev/null
cd x86probe
rm -f tests/*.cpp 2>/dev/null || true

# ⚠️ NO `[target.*]` SECTION. Like `aarch64-none-elf`, the zero-libc tier is
# this row's own property rather than something the project asks for.
cat > mcpp.toml <<'EOF'
[package]
name    = "x86probe"
version = "0.1.0"

[build]
target = "x86_64-none-elf"
EOF

# The console on this machine is not memory: x86 reaches a serial port through
# `out`, which is why the other two probes' `volatile unsigned*` has no
# counterpart here.
cat > src/main.cpp <<'EOF'
namespace {
inline void outb(unsigned short port, unsigned char v) {
    asm volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
}
extern "C" void kmain() {
    for (const char* p = "x86 ok\n"; *p; ++p) outb(0x3F8, static_cast<unsigned char>(*p));
    for (;;) { asm volatile("hlt"); }
}
asm(".section .text.entry,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "  movq $__stack_top, %rsp\n"
    "  call kmain\n"
    "1: hlt\n"
    "  jmp 1b\n");
EOF

cat > link.ld <<'EOF'
ENTRY(_start)
SECTIONS {
    . = 0x100000;
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

# ── 1. It builds, which for this row means the link did not go through gcc ──
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "x86_64-none-elf did not build"; exit 1; }

ELF=$(find target/x86_64-none-elf -type f -name x86probe | head -1)
[ -n "$ELF" ] || { cat build.log; echo "no image was produced"; exit 1; }

READELF=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm" -name llvm-readelf 2>/dev/null | head -1)
NM=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm" -name llvm-nm 2>/dev/null | head -1)
[ -x "$READELF" ] && [ -x "$NM" ] || { echo "the LLVM payload has no readelf/nm to check with"; exit 1; }

# ── 2. It is x86-64, and it starts where the script says ────────────────────
"$READELF" -h "$ELF" | grep -q "X86-64" || {
    "$READELF" -h "$ELF" | head -12; echo "the image is not x86-64"; exit 1; }
"$READELF" -S "$ELF" | grep -E '\.text' | grep -q '0000000000100000' || {
    "$READELF" -S "$ELF" | head -8
    echo "the linker script's load address did not reach the image"; exit 1; }

# ── 3. Nothing is left undefined, and nothing names a loader ────────────────
UNDEF=$("$NM" -u "$ELF" | wc -l)
[ "$UNDEF" -eq 0 ] || {
    "$NM" -u "$ELF"; echo "the image expects $UNDEF symbols nothing provides"; exit 1; }

# ⚠️ An image with no operating system under it must not name an interpreter.
# The direct-linker path says `--no-dynamic-linker` where the driver path said
# `-static`; a PT_INTERP here would be that substitution having been dropped,
# and it is exactly the defect the `--no-default-config` comment in flags.cppm
# records for the riscv row: it links clean and reports success.
if "$READELF" -l "$ELF" | grep -q "INTERP"; then
    "$READELF" -l "$ELF" | head -12
    echo "a bare-metal image names a dynamic loader"; exit 1
fi

# ── 4. The link map was produced, through a linker-native flag ──────────────
# `-Wl,-Map=` is how a DRIVER is asked for a map. Handed to `ld.lld` it is an
# unknown option, so the map's existence is evidence that the flag was rendered
# for the tool actually being invoked.
[ -f "$ELF.map" ] || { ls -la "$(dirname "$ELF")"; echo "no link map beside the image"; exit 1; }

# ── 5. The host toolchain was not consulted ─────────────────────────────────
# The load-bearing assertion for this row. A link that had fallen back to the
# driver would name `g++` — and would then work on this machine and on no
# other, which is the failure this row exists to prevent.
rm -rf target
"$MCPP" build --verbose > verbose.log 2>&1 || { cat verbose.log; exit 1; }
grep -q "ld.lld" verbose.log || {
    grep -iE "link|ld" verbose.log | tail -5
    echo "the link did not go through ld.lld"; exit 1; }

# ── 6. The ISA profile is the measured one ──────────────────────────────────
# `-march=x86-64` with a HYPHEN, where the triple segment has an underscore;
# substituting one for the other produces `unknown target CPU`. And
# `-mno-red-zone`, without which any trap handler on this target corrupts the
# leaf function it interrupted, with no diagnostic.
grep -q -- "-march=x86-64" verbose.log || {
    grep -o -- '-march=[a-z0-9_-]*' verbose.log | sort -u
    echo "the x86_64 ISA profile is not the measured one"; exit 1; }
grep -q -- "-mno-red-zone" verbose.log || {
    echo "the red zone was left enabled on a target that takes interrupts"; exit 1; }

echo "PASS: x86_64-none-elf builds on the zero-libc tier, linked by ld.lld directly"
