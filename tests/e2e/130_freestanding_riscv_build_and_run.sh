#!/usr/bin/env bash
# requires-hard: llvm qemu-riscv unix-shell
# Bare metal, end to end: `mcpp build --target riscv64-none-elf` produces a
# RISC-V firmware image from a C++20 MODULE interface unit plus assembly, and
# `mcpp run --target-triple` boots it in qemu.
#
# ⚠️ `requires-hard`, not `requires`. Every capability here is one a runner
# that runs this file is supposed to have: llvm is mcpp's own payload and
# qemu-riscv is an xim package. Declared the soft way, a runner that lost
# either would report SKIP — indistinguishable from "inapplicable on this
# platform" — and the one test that proves the bare-metal chain works would
# stop running while the job stayed green. That has happened twice in this
# repository already.
#
# What each assertion is FOR (none of them is decoration):
#
#   UCB RISC-V          the artifact is for the target, not the host. Before
#                       the retargetable-driver fix, `--target riscv64-none-elf`
#                       resolved clang, reported success, and emitted an x86-64
#                       binary into target/x86_64-linux-gnu/.
#   no PT_INTERP        no dynamic loader was requested. The llvm payload's
#                       clang.cfg carries an unconditional
#                       `-Wl,--dynamic-linker=…/ld-linux-x86-64.so.2`, and a
#                       freestanding link that loses `--no-default-config`
#                       bakes an x86-64 interpreter into a RISC-V image — which
#                       links clean and reports success.
#   no undefined syms   nothing was left for a libc that is not there.
#   entry == 0x80200000 the linker script was honoured; qemu `virt` loads here.
#   the printed line    the MODULE actually executed on the emulated hart.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new fw > /dev/null
cd fw
rm -f src/main.cpp

cat > src/start.S <<'EOF'
.section .text.start
.globl _start
_start:
    la sp, stack_top
    call kmain
1:  wfi
    j 1b
.section .bss
.align 12
stack:
    .space 4096
stack_top:
EOF

cat > src/kmain.cppm <<'EOF'
export module kmain;
namespace {
    // qemu `virt`'s 16550 UART.
    volatile char* const kUart = reinterpret_cast<volatile char*>(0x10000000);
    void put(char c) { *kUart = c; }
}
export extern "C" void kmain() {
    for (const char* s = "MCPP-FREESTANDING-OK\n"; *s; ++s) put(*s);
    // Poweroff through the virt machine's syscon, so the run ends on the
    // firmware's terms rather than on a timeout.
    *reinterpret_cast<volatile unsigned int*>(0x100000) = 0x5555;
}
EOF

cat > link.ld <<'EOF'
ENTRY(_start)
SECTIONS {
  . = 0x80200000;
  .text   : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(.bss*) *(COMMON) }
}
EOF

cat > mcpp.toml <<EOF
[package]
name    = "fw"
version = "0.1.0"

[build]
ldflags = ["-T", "$TMP/fw/link.ld"]

[targets.firmware]
kind = "bin"
main = "src/start.S"

[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-no-reboot", "-bios", "default", "-kernel"]
EOF

# ── build ───────────────────────────────────────────────────────────────────
"$MCPP" build --target riscv64-none-elf > build.log 2>&1 || {
    cat build.log; echo "bare-metal build failed"; exit 1; }

img="$(find target/riscv64-none-elf -name firmware -type f | head -1)"
[[ -n "$img" ]] || {
    echo "no artifact under target/riscv64-none-elf/ — built for the wrong target?"
    find target -maxdepth 1 -type d; exit 1; }

file "$img" | grep -q 'UCB RISC-V' || {
    echo "artifact is not a RISC-V image: $(file "$img")"; exit 1; }

# `if`, not `! cmd | grep -q`: a `!`-prefixed pipeline is exempt from errexit
# and would never fail the test.
if readelf -l "$img" | grep -q INTERP; then
    echo "artifact requests a dynamic interpreter:"
    readelf -l "$img" | grep -A1 INTERP
    exit 1
fi

LLVM_NM=""
for d in "$HOME"/.mcpp/registry/data/xpkgs/xim-x-llvm/*/bin \
         "$HOME"/.xlings/data/xpkgs/xim-x-llvm/*/bin; do
    [[ -x "$d/llvm-nm" ]] && LLVM_NM="$d/llvm-nm"
done
if [[ -n "$LLVM_NM" ]]; then
    undef="$("$LLVM_NM" -u "$img" | wc -l)"
    [[ "$undef" -eq 0 ]] || {
        echo "artifact has $undef undefined symbols:"; "$LLVM_NM" -u "$img"; exit 1; }
fi

readelf -h "$img" | grep -q '0x80200000' || {
    echo "entry point is not 0x80200000 — the linker script was not applied:"
    readelf -h "$img" | grep -i entry; exit 1; }

# ── run ─────────────────────────────────────────────────────────────────────
"$MCPP" run --target-triple riscv64-none-elf > run.log 2>&1 || true
grep -q 'MCPP-FREESTANDING-OK' run.log || {
    cat run.log; echo "the firmware did not run (or produced no output)"; exit 1; }

# ── the runner is REQUIRED, and its absence must say so ─────────────────────
# Two-sided: without this, "run worked" could equally mean mcpp exec'd the
# image directly and something else printed the line.
python3 - "$TMP/fw/mcpp.toml" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
t = p.read_text()
i = t.index("[target.riscv64-none-elf]")
p.write_text(t[:i])
PY
if "$MCPP" run --target-triple riscv64-none-elf > norunner.log 2>&1; then
    cat norunner.log
    echo "running with no runner configured should have failed"
    exit 1
fi
grep -q 'no runner is configured' norunner.log || {
    cat norunner.log; echo "missing-runner diagnostic did not name the problem"; exit 1; }
grep -q 'target.riscv64-none-elf' norunner.log || {
    cat norunner.log; echo "diagnostic did not name the manifest key to add"; exit 1; }

echo "PASS: freestanding riscv64 build + run"
