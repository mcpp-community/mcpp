#!/usr/bin/env bash
# requires: llvm unix-shell
# `[target.<triple>].sysroot` and the three target queries a board package asks.
#
# ⚠️ WHY THIS TEST IS TWO-SIDED THROUGHOUT
#
# Both features here are of the kind whose presence and absence look identical
# on a machine that is already configured. A sysroot override that silently did
# nothing would still build, because the target row's picolibc is what would be
# used either way; a query that returned a stale value would still compile,
# because there is only one toolchain and one C library installed. So every
# assertion below pins BOTH states — with the override and without it, with a C
# library and without one.
#
# The queries exist to remove a coupling that no manifest shows. `riscv-virt-rt`
# declares no dependency on LLVM and none on picolibc, and still could not serve
# a second toolchain or a second C library, because it had written
# `clang_rt.builtins-riscv64` and `rv64gc/lp64d` into its build program.
#
# No emulator is needed: everything here is decided at configure and link time,
# which is why `requires:` asks only for llvm.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new probe > /dev/null
cd probe
rm -f tests/*.cpp 2>/dev/null || true

# A build program that reports what the engine told it. It returns non-zero so
# that mcpp surfaces the output: a build program's stderr is otherwise only
# shown when it fails, and a probe nobody can read proves nothing.
cat > build.mcpp <<'EOF'
import mcpp;
import std;
int main() {
    std::cerr << "builtins=[" << (mcpp::target_builtins_lib() ?: "") << "]\n"
              << "profile=["  << (mcpp::target_libc_profile() ?: "") << "]\n"
              << "libc=["     << (mcpp::target_libc()         ?: "") << "]\n";
    return 1;
}
EOF
cat > src/main.cpp <<'EOF'
extern "C" void _start() { for (;;) {} }
EOF

manifest() {
    cat > mcpp.toml <<EOF
[package]
name    = "probe"
version = "0.1.0"

[build]
target = "riscv64-none-elf"
$1
EOF
}

# ── 1. The target row's C library, and the queries derived from it ───────────
manifest ""
"$MCPP" build > q64.log 2>&1 || true
grep -q 'builtins=\[clang_rt.builtins-riscv64\]' q64.log || {
    cat q64.log; echo "rv64 builtins query wrong"; exit 1; }
grep -q 'profile=\[rv64gc/lp64d\]' q64.log || {
    cat q64.log; echo "rv64 libc profile query wrong"; exit 1; }
grep -q 'libc=\[picolibc-riscv\]' q64.log || {
    cat q64.log; echo "libc name query wrong"; exit 1; }

# ── 2. The SAME manifest at the other width ─────────────────────────────────
# This is what makes the queries worth having: one board description, two ISA
# profiles, and the varying parts come from the engine rather than from a
# branch in the package.
"$MCPP" build --target riscv32-none-elf > q32.log 2>&1 || true
grep -q 'builtins=\[clang_rt.builtins-riscv32\]' q32.log || {
    cat q32.log; echo "rv32 builtins query did not follow the target"; exit 1; }
grep -q 'profile=\[rv32imac/ilp32\]' q32.log || {
    cat q32.log; echo "rv32 libc profile query did not follow the target"; exit 1; }

# ── 3. The C library is really reachable without the override ───────────────
# Establishes the control for step 4: `<stdio.h>` resolves here.
cat > src/main.cpp <<'EOF'
#include <stdio.h>
extern "C" void _start() { (void)sizeof(FILE); for (;;) {} }
EOF
cat > build.mcpp <<'EOF'
import mcpp;
int main() { return 0; }
EOF
manifest ""
"$MCPP" build > with_libc.log 2>&1 || {
    cat with_libc.log; echo "the target row's C library should have been usable"; exit 1; }

# ── 4. `sysroot = ""` really removes it ─────────────────────────────────────
# ⚠️ THE LOAD-BEARING ASSERTION. Without it, an override that parsed and did
# nothing would pass every other check in this file.
manifest '
[target.riscv64-none-elf]
sysroot = ""'
if "$MCPP" build > no_libc.log 2>&1; then
    cat no_libc.log
    echo "sysroot = \"\" did not remove the C library — <stdio.h> still resolved"
    exit 1
fi
grep -q "stdio.h" no_libc.log || {
    cat no_libc.log; echo "build failed, but not because the C library was gone"; exit 1; }

# ── 5. The zero-libc tier still produces an image ───────────────────────────
# Removing the C library must leave a usable target, not a broken one.
cat > src/main.cpp <<'EOF'
static volatile unsigned char* const UART =
    reinterpret_cast<unsigned char*>(0x10000000);
extern "C" void kmain() {
    for (const char* p = "zero-libc\n"; *p; ++p)
        *UART = static_cast<unsigned char>(*p);
    *reinterpret_cast<volatile unsigned int*>(0x100000) = 0x5555;
    for (;;) {}
}
asm(".section .text.entry\n.globl _start\n_start:\n"
    "  la sp, __stack_top\n  call kmain\n1: j 1b\n");
EOF
cat > link.ld <<'EOF'
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text : { *(.text.entry) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16); . = . + 0x1000; __stack_top = .;
}
EOF
manifest "ldflags = [\"-T\", \"$PWD/link.ld\"]

[target.riscv64-none-elf]
sysroot = \"\""
"$MCPP" build > zero.log 2>&1 || {
    cat zero.log; echo "a self-contained zero-libc image should still build"; exit 1; }
grep -q 'Size probe' zero.log || {
    cat zero.log; echo "no size summary — the freestanding link path was not taken"; exit 1; }

# ── 6. On the zero-libc tier the libc-facing queries are empty ──────────────
# All three answers move together: a kernel must not be handed a path into a
# C library that is not there, while `builtins` is a COMPILER fact and stays.
cat > build.mcpp <<'EOF'
import mcpp;
import std;
int main() {
    std::cerr << "builtins=[" << (mcpp::target_builtins_lib() ?: "") << "]\n"
              << "profile=["  << (mcpp::target_libc_profile() ?: "") << "]\n"
              << "libc=["     << (mcpp::target_libc()         ?: "") << "]\n";
    return 1;
}
EOF
"$MCPP" build > zeroq.log 2>&1 || true
grep -q 'profile=\[\]' zeroq.log || {
    cat zeroq.log; echo "libc profile should be empty with no C library"; exit 1; }
grep -q 'libc=\[\]' zeroq.log || {
    cat zeroq.log; echo "libc name should be empty with no C library"; exit 1; }
grep -q 'builtins=\[clang_rt.builtins-riscv64\]' zeroq.log || {
    cat zeroq.log; echo "builtins is a compiler fact and must survive"; exit 1; }

# ── 7. A bare name is rejected at parse time ────────────────────────────────
# Accepting it would install nothing and fail much later naming a missing libc.
manifest '
[target.riscv64-none-elf]
sysroot = "newlib"'
if "$MCPP" build > badref.log 2>&1; then
    cat badref.log; echo "a bare sysroot name should have been rejected"; exit 1; fi
grep -q 'xpkg reference' badref.log || {
    cat badref.log; echo "rejection did not explain the expected form"; exit 1; }

echo "PASS: [target.X].sysroot override, zero-libc tier, and the three target queries"
