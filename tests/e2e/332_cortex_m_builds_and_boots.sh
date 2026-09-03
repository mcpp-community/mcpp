#!/usr/bin/env bash
# requires: llvm unix-shell qemu-arm
# Cortex-M: the M-profile rows build and boot, and honour the float ABI.
#
# ⚠️ THE PROPERTY THAT MATTERS HERE IS THE ONE A REASONED TABLE WOULD HAVE GOT
# WRONG:
#
#   * the soft-float rows do not emit FPU instructions. `thumbv7em`'s
#     architecture implies an FPU, so clang emits `vmul.f32` for a float
#     multiply under the soft-float ABI as readily as under the hard one; on a
#     Cortex-M4 without an FPU that faults at run time, with a clean compile and
#     a clean link. The row carries `-mfpu=none` for this, and the assertion is
#     an instruction count.
#
# ⚠️ AND THE IMAGE IS RUN, NOT INSPECTED. A freestanding image that links is not
# evidence: the entry point and the ordering of the vector table are only
# exercised by a machine that fetches from address zero.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

qemu_arm() {
    local d
    for d in "${MCPP_HOME:-$HOME/.mcpp}/registry" "$HOME/.xlings"; do
        local c
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
NM="$(llvm_tool llvm-nm)"       || { echo "SKIP: llvm-nm not found"; exit 0; }
OBJDUMP="$(llvm_tool llvm-objdump)" || { echo "SKIP: llvm-objdump not found"; exit 0; }

mkdir -p "$work/mcu/src"
cd "$work/mcu"

cat > mcpp.toml <<'TOML'
[package]
name    = "mcu"
version = "0.1.0"
TOML

# ⚠️ `volatile` on the operands. Without it the multiply is constant-folded and
# the FPU assertion below passes for a reason unrelated to the flag.
cat > src/main.cpp <<'CPP'
namespace {
inline void sh(int op, const void* a) {
    register int r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = a;
    __asm__ volatile("bkpt 0xAB" :: "r"(r0), "r"(r1) : "memory");
}
}
extern "C" unsigned __stack_top;

extern "C" void Reset_Handler() {
    sh(0x04, (void*)"cortex-m ok\n");
    sh(0x18, (void*)0x20026);          // semihosting SYS_EXIT, ADP_Stopped_ApplicationExit
    for (;;) {}
}

// Referenced by nothing — the hardware reads it by address, which is why the
// linker script says KEEP.
extern "C" __attribute__((section(".vectors"), used))
void* const vectors[] = { (void*)&__stack_top, (void*)Reset_Handler };
CPP

cat > build.mcpp <<'BUILD'
import mcpp;
int main() { mcpp::link_script("link.ld"); return 0; }
BUILD

# The memory map is a BOARD fact, not a target fact — which is why it is written
# here per machine rather than derived from the triple.
emit_ld() {
    cat > link.ld <<EOF
ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx)  : ORIGIN = $1, LENGTH = $2
  RAM   (rwx) : ORIGIN = $3, LENGTH = $4
}
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM
  .bss  : { *(.bss*) *(COMMON) } > RAM
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
EOF
}

ran=0
boot_row() {              # triple machine cpuflag flash_org flash_len ram_org ram_len
    local triple=$1 machine=$2 cpuflag=$3
    emit_ld "$4" "$5" "$6" "$7"
    rm -rf target
    "$MCPP" build --target "$triple" >/dev/null 2>&1 || {
        echo "FAIL: $triple did not build"; exit 1; }
    local elf
    elf=$(find target -type f -name mcu | head -1)
    [ -n "$elf" ] || { echo "FAIL: $triple produced no artefact"; exit 1; }

    # The vector table is present: it is what the machine fetches at reset, and
    # an image without it boots into nothing.
    if [ "$("$NM" "$elf" | grep -c vectors)" != "1" ]; then
        echo "FAIL: $triple has no vector table"; exit 1
    fi

    local out
    out=$(timeout 30 "$QEMU" -machine "$machine" $cpuflag -nographic -semihosting \
                             -no-reboot -kernel "$elf" 2>&1 | head -3)
    case "$out" in
        *"cortex-m ok"*) ;;
        *) echo "FAIL: $triple did not boot on $machine; got: $out"; exit 1 ;;
    esac
    echo "  ok  $triple booted on $machine"
    ran=$((ran + 1))
}

# The four rows the target table marks `verified`, each on the machine that
# earned it. nRF51 has 16K of RAM; a generic map hard-faults before `main`.
boot_row thumbv6m-none-eabi       microbit   ""                0x00000000 256K 0x20000000 16K
boot_row thumbv7m-none-eabi       mps2-an385 "-cpu cortex-m3"  0x00000000 4M   0x20000000 4M
boot_row thumbv7em-none-eabihf    mps2-an386 "-cpu cortex-m4"  0x00000000 4M   0x20000000 4M
boot_row thumbv8m.main-none-eabi  mps2-an505 ""                0x10000000 4M   0x38000000 512K

# ⚠️ A COUNT, BECAUSE A LOOP THAT RAN ZERO TIMES ALSO REACHES THIS LINE.
[ "$ran" = "4" ] || { echo "FAIL: expected 4 rows to boot, got $ran"; exit 1; }

# ── The float ABI, asserted on both sides of the pair ──────────────────────
#
# ⚠️ A SEPARATE PROJECT, AND THAT SEPARATION IS ITSELF A MEASUREMENT. Float
# arithmetic cannot live in the fixture above: on a soft-float row it lowers
# onto `__aeabi_fmul`, and this tier has no C library and no builtins to resolve
# it against, so every boot row would fail to link. The boot fixture is
# therefore integer-only and the float question is asked here.
#
# ⚠️ AND ONE SIDE ALONE PROVES NOTHING. "The soft row emitted no FPU
# instruction" is equally true of a build that emitted no float code, so the
# hard row is measured from the same source as the control.
mkdir -p "$work/fp/src"
cd "$work/fp"
cp "$work/mcu/build.mcpp" .
printf '[package]\nname = "fp"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
extern "C" unsigned __stack_top;
// `volatile`, so the multiply survives constant folding and is really emitted.
volatile float fa = 3.0f, fb = 4.0f, fout = 0.0f;
extern "C" void Reset_Handler() { fout = fa * fb + 1.0f; for (;;) {} }
extern "C" __attribute__((section(".vectors"), used))
void* const vectors[] = { (void*)&__stack_top, (void*)Reset_Handler };
CPP
emit_ld 0x00000000 4M 0x20000000 4M

rm -rf target
"$MCPP" build --target thumbv7em-none-eabihf >/dev/null 2>&1 || {
    echo "FAIL: the hard-float row did not build"; exit 1; }
hard=$("$OBJDUMP" -d "$(find target -type f -name fp | head -1)" \
       | grep -cE '\bv[a-z]+\.f32' || true)
[ "$hard" -gt 0 ] || {
    echo "FAIL: thumbv7em-none-eabihf emitted no FPU instruction — the control is vacuous"; exit 1; }
echo "  ok  thumbv7em-none-eabihf uses the FPU ($hard instructions)"

# ⭐ The soft row's proof is its LINK FAILURE, and that is the strongest form
# available at this tier. `-mfpu=none` makes the compiler lower the multiply
# onto `__aeabi_fmul` rather than `vmul.f32`; with no C library and no builtins
# there is nothing for that call to resolve against. A link that fails naming
# `__aeabi_fmul` therefore states exactly what is asserted: the multiply did
# NOT become an FPU instruction.
rm -rf target
soft_out=$("$MCPP" build --target thumbv7em-none-eabi 2>&1 || true)
case "$soft_out" in
    *__aeabi_fmul*) echo "  ok  thumbv7em-none-eabi lowered the multiply onto a libcall, not the FPU" ;;
    *) echo "FAIL: thumbv7em-none-eabi did not reference __aeabi_fmul; -mfpu=none may not be applied"
       echo "$soft_out" | tail -5 | sed 's/^/      /'; exit 1 ;;
esac

echo "PASS: cortex-m rows build, boot and honour the float ABI"
