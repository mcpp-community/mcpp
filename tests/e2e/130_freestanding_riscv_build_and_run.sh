#!/usr/bin/env bash
# requires: llvm qemu-riscv unix-shell
# Bare metal, end to end: `mcpp build --target riscv64-none-elf` produces a
# RISC-V firmware image from a C++20 MODULE interface unit plus assembly, and
# `mcpp run --target-triple` boots it in qemu.
#
# ⚠️ SOFT `requires:`, and the guard lives elsewhere on purpose.
#
# `requires-hard:` (missing capability FAILS instead of skipping) exists and is
# the right tool for "this runner is misconfigured" — but it cannot express
# THIS test's condition. Neither `llvm` nor `qemu-riscv` is present on the
# macOS and Windows e2e runners, so a hard token here makes those jobs
# structurally red forever — measured, not predicted: this file shipped with
# `requires-hard` for one round and the macOS suite reported
#
#     FAIL: 130_freestanding_riscv_build_and_run.sh
#           (REQUIRED capability missing: llvm)
#
# which is a worse failure than the one it was meant to prevent.
#
# The thing that must not happen — this test silently skipping on the runner
# that is supposed to run it, leaving the bare-metal chain unexercised while
# the job stays green — is guarded in ci-linux-e2e.yml, which installs the
# capabilities and then asserts this test's PASS line actually appeared. A
# guard belongs where it can be exact about which runner it is talking about.
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

# Resolve the emulator to a PATH-independent absolute path.
#
# The runner template may name it bare — that is what a user writes — but this
# test is about mcpp's runner MECHANISM, not about shim/home topology. Measured
# in CI: `qemu-system-riscv64 --version` succeeded in one step while `mcpp run`
# execing the same bare name answered "xlings: 'qemu-system-riscv64' is not
# installed", because the shim on PATH dispatches against its owner home. A
# real BSP has the same information and would emit an absolute path too.
QEMU="$(command -v qemu-system-riscv64 || true)"
for d in "$HOME"/.mcpp/registry/data/xpkgs/*-x-qemu-riscv/*/bin \
         "$HOME"/.xlings/data/xpkgs/*-x-qemu-riscv/*/bin; do
    [[ -x "$d/qemu-system-riscv64" ]] && QEMU="$d/qemu-system-riscv64"
done
[[ -n "$QEMU" ]] || { echo "SKIP: no qemu-system-riscv64"; exit 0; }

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
runner = ["QEMU_PATH", "-machine", "virt", "-nographic",
          "-no-reboot", "-bios", "default", "-kernel"]
EOF
sed -i "s|QEMU_PATH|$QEMU|" mcpp.toml

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
# ⚠️ BOTH spellings, and the pairing is the test.
#
# `run` also takes a POSITIONAL binary name, and the arg parser falls back
# from an unset option to a positional of the same name. While that positional
# was itself called `target`, adding `--target` here silently turned
# `mcpp run <binary>` into `mcpp run --target=<binary>`:
#
#     $ mcpp run q
#     error: unknown target 'q'
#
# `--target-triple` shipped first and stays an alias, so both are pinned; the
# positional case is pinned by 73_issue131_per_target_cxxflag.sh.
"$MCPP" run --target riscv64-none-elf > run.log 2>&1 || true
grep -q 'MCPP-FREESTANDING-OK' run.log || {
    cat run.log; echo "the firmware did not run (or produced no output)"; exit 1; }

"$MCPP" run --target-triple riscv64-none-elf > alias.log 2>&1 || true
grep -q 'MCPP-FREESTANDING-OK' alias.log || {
    cat alias.log; echo "--target-triple (the 2026.8.19.1 spelling) stopped working"; exit 1; }

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
