#!/usr/bin/env bash
# requires: llvm unix-shell qemu-arm
# ARMv7-A: the first 32-bit row in the table with a memory management unit.
#
# ⚠️ THE ROW IS NOT A SECOND SPELLING OF THE M ROWS, AND THIS TEST IS WHERE THAT
# BECOMES CHECKABLE. Every other 32-bit target here is M-profile — an MPU that
# describes regions by base and limit, with no page-table entry at all — so it
# is the one machine class on which an address-space abstraction has never been
# asked what a 32-bit entry looks like.
#
# ⚠️ AND THE SEMIHOSTING EXIT CALL IS SPELLED DIFFERENTLY FROM M-PROFILE.
# `SYS_EXIT` (0x18) on AArch32 takes the reason code in `r1` DIRECTLY; the
# `{reason, code}` block a Cortex-M board passes is `SYS_EXIT_EXTENDED` (0x20).
# Measured: passing the block to 0x18 prints correctly and then reports the
# wrong exit status. The assertion below reads the STATUS, not only the output,
# which is what makes that difference visible here rather than on a board.
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
NM="$(llvm_tool llvm-nm)" || { echo "SKIP: llvm-nm not found"; exit 0; }
OBJDUMP="$(llvm_tool llvm-objdump)" || { echo "SKIP: llvm-objdump not found"; exit 0; }

mkdir -p "$work/soc/src"
cd "$work/soc"
printf '[package]\nname = "soc"\nversion = "0.1.0"\n' > mcpp.toml

cat > src/start.S <<'ASM'
.section .text.start,"ax"
.global _start
_start:
    ldr sp, =__stack_top
    bl  kmain
1:  b 1b
ASM

cat > src/main.cpp <<'CPP'
namespace {
inline void sh(int op, const void* a) {
    register int r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = a;
    __asm__ volatile("svc 0x123456" :: "r"(r0), "r"(r1) : "memory");
}
}
// Referenced by nothing. --gc-sections must remove it.
extern "C" void collected_because_nothing_calls_it() { sh(0x04, (void*)"UNREACHABLE\n"); }

extern "C" void kmain() {
    sh(0x04, (void*)"armv7a ok\n");
    // ADP_Stopped_ApplicationExit, in r1 itself — see the header note.
    sh(0x18, (void*)0x20026);
    for (;;) {}
}
CPP

cat > build.mcpp <<'BUILD'
import mcpp;
int main() { mcpp::link_script("link.ld"); return 0; }
BUILD

# qemu `-M virt` puts RAM at 0x40000000. A board fact, written here.
cat > link.ld <<'LD'
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 16M }
SECTIONS {
  .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } > RAM
  .data : { *(.data*) } > RAM
  .bss  : { *(.bss*) *(COMMON) } > RAM
  . = ALIGN(16);
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
LD

ran=0
boot_row() {
    local triple=$1
    rm -rf target
    "$MCPP" build --target "$triple" >/dev/null 2>&1 || {
        echo "FAIL: $triple did not build"; exit 1; }
    local elf
    elf=$(find target -type f -name soc | head -1)
    [ -n "$elf" ] || { echo "FAIL: $triple produced no artefact"; exit 1; }
    if [ "$("$NM" "$elf" | grep -c collected_because_nothing_calls_it)" != "0" ]; then
        echo "FAIL: $triple kept a function nothing calls (--gc-sections not applied)"; exit 1
    fi
    local out rc
    # ⚠️ NOT `qemu | head`, AND THAT IS THE WHOLE POINT OF THE STATUS CHECK.
    # `$?` after a pipeline is the LAST command's status, so piping into `head`
    # would read head's 0 and the exit assertion below would be vacuous — this
    # repository has shipped that shape before. The output goes to a file and
    # the status is taken from qemu itself.
    set +e
    timeout 30 "$QEMU" -M virt -cpu cortex-a15 -nographic -semihosting \
                       -no-reboot -kernel "$elf" > qemu.log 2>&1
    rc=$?
    set -e
    out=$(head -3 qemu.log)
    case "$out" in
        *"armv7a ok"*) ;;
        *) echo "FAIL: $triple did not boot; got: $out"; exit 1 ;;
    esac
    # ⭐ THE EXIT STATUS, NOT ONLY THE OUTPUT. A program that prints and then
    # gets its exit call wrong is exactly what this row's semihosting note is
    # about, and only the status tells the two apart.
    [ "$rc" = "0" ] || { echo "FAIL: $triple booted but exited $rc"; exit 1; }
    echo "  ok  $triple booted on virt/cortex-a15 and exited 0"
    ran=$((ran + 1))
}

boot_row armv7a-none-eabi
boot_row armv7a-none-eabihf
[ "$ran" = "2" ] || { echo "FAIL: expected 2 rows to boot, got $ran"; exit 1; }

# ── The float ABI, both sides, on this architecture ────────────────────────
#
# ⚠️ MEASURED HERE RATHER THAN CARRIED OVER FROM M-PROFILE. `armv7-a` is a
# different architecture from `thumbv7em`; that the soft ABI still reaches the
# FPU there says nothing about here. It does — measured — and the row carries
# `-mfpu=none` for it.
mkdir -p "$work/fp/src"
cd "$work/fp"
cp "$work/soc/build.mcpp" "$work/soc/link.ld" .
cp "$work/soc/src/start.S" src/
printf '[package]\nname = "fp"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
volatile float fa = 3.0f, fb = 4.0f, fout = 0.0f;
extern "C" void kmain() { fout = fa * fb + 1.0f; for (;;) {} }
CPP

rm -rf target
"$MCPP" build --target armv7a-none-eabihf >/dev/null 2>&1 || {
    echo "FAIL: the hard-float row did not build"; exit 1; }
hard=$("$OBJDUMP" -d "$(find target -type f -name fp | head -1)" \
       | grep -cE '\bv[a-z]+\.f32' || true)
[ "$hard" -gt 0 ] || {
    echo "FAIL: armv7a-none-eabihf emitted no FPU instruction — the control is vacuous"; exit 1; }
echo "  ok  armv7a-none-eabihf uses the FPU ($hard instructions)"

# The soft row's proof is its link failure against `__aeabi_fmul`: with
# `-mfpu=none` the multiply lowers onto a libcall, and this tier has no C
# library and no builtins for it to resolve against.
rm -rf target
soft_out=$("$MCPP" build --target armv7a-none-eabi 2>&1 || true)
case "$soft_out" in
    *__aeabi_fmul*) echo "  ok  armv7a-none-eabi lowered the multiply onto a libcall, not the FPU" ;;
    *) echo "FAIL: armv7a-none-eabi did not reference __aeabi_fmul; -mfpu=none may not be applied"
       echo "$soft_out" | tail -5 | sed 's/^/      /'; exit 1 ;;
esac

echo "PASS: armv7-a rows build, boot, exit cleanly and honour the float ABI"
