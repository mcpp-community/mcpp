#!/usr/bin/env bash
# requires: llvm unix-shell qemu-riscv
# openkal where there is no operating system, and no C library either.
#
# ⚠️ THE FREESTANDING SHAPE IS THE ONE THE ENGINE MODELS MOST AND TESTS LEAST.
# `riscv64-none-elf` appears in ten e2e scripts and in none of them does a real
# openkal package supply the platform: they build a program with no dependency
# at all, which exercises the freestanding link path and not the seam this
# ecosystem exists for. The layer that changes here is `kernelAbi` — supplied
# by openkal-opensbi, which answers to the SBI a machine's firmware provides —
# while `cAbi` is genuinely absent.
#
# ⭐ `cAbi.absent()` IS A DISTINCT ORIGIN, AND THE LINK LINE BRANCHES ON IT.
# A hosted target whose C library comes from a package and one that has no C
# library at all take different flags (`-nostdlib -static` for the latter), and
# the predicate that chooses between them was, until 2026-08-25, an OR over two
# layers. Four origins, four behaviours; this covers the one no other e2e does.
#
# ⚠️ AND THE PROGRAM RUNS UNDER QEMU RATHER THAN MERELY LINKING. A freestanding
# image that links is not evidence: the entry point, the linker script and the
# ordering of the startup objects are BSP facts, and the only check that reaches
# them is the machine printing what the program told it to.
set -e

MCPP="${MCPP:-mcpp}"
TARGET=riscv64-none-elf
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app/src"
cd "$work/app"

cat > mcpp.toml <<'TOML'
[package]
name    = "okbare"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

# ⚠️ `sysroot = ""` IS WHAT MAKES THIS THE NO-C-LIBRARY SHAPE, AND THE TARGET
# NAME DOES NOT IMPLY IT.
#
# `riscv64-none-elf` names a machine with no operating system; whether the
# program has a C library is a separate statement, and the target table answers
# it with a default — `xim:picolibc-riscv@1.8.12`. Measured while writing this
# file, whose whole point is the layer being ABSENT:
#
#     c-abi  picolibc-riscv (xim:picolibc-riscv@1.8.12, prebuilt)
#
# So the absence has to be asked for. This is the one e2e that covers
# `cAbi.absent()`, the fourth `Origin` value, and it would have been covering
# `Xpkg` instead.
[target.riscv64-none-elf]
sysroot = ""

# The platform, and nothing above it. `standalone` says this implementation is
# the whole of the program's environment: it supplies the entry point, because
# no C runtime is going to.
[dependencies]
openkal-opensbi = { version = "0.1.5", features = ["standalone"] }
TOML

cat > src/main.cpp <<'CPP'
import openkal.stream;
import openkal.abort;

// No <cstdio>: there is no C library here. The only way out of this program is
// the interface the platform package implements.
// ⚠️ `extern "C"`, AND THE THREE-ARGUMENT SIGNATURE THE ENTRY POINT DECLARES.
// `-ffreestanding` removes the compiler's special handling of `main`, so the
// name is an ordinary symbol and has to match what calls it. The platform
// package's `standalone` feature runs the initialisers and then calls
// `main(0, &nothing, &nothing)` — see `__okb_start_c` in its src/start.cpp. A
// plain `int main()` compiled as C++ mangles to something else entirely:
//
//     ld.lld: error: undefined symbol: main
extern "C" int main(int, char**, char**) {
    const char msg[] = "okbare alive\n";
    kal_stream_write(kal_stdout(), msg, sizeof msg - 1);
    return 0;
}
CPP

if ! out="$("$MCPP" build --target "$TARGET" 2>&1)"; then
    case "$out" in
      *"not found in the synced index"*|*"install_packages failed"*)
        echo "SKIP: openkal-opensbi is not reachable from here"; exit 0 ;;
      *"cannot be built on this host"*)
        echo "SKIP: $TARGET cannot be built here"; exit 0 ;;
    esac
    echo "FAIL: the freestanding openkal build failed"
    printf '%s\n' "$out" | grep -iE 'error' | head -5
    exit 1
fi

# ── The report says what this target side is, and is not ───────────────────
case "$out" in
  *"kernel-abi"*graph*) echo "  ok  kernel-abi from the graph" ;;
  *) echo "FAIL: the platform did not come from the graph"
     printf '%s\n' "$out" | grep -E 'abi' | sed 's/^/        /'; exit 1 ;;
esac
# ⭐ AND THE `c-abi` LINE NAMES NOTHING.
#
# ⚠️ THE FIRST VERSION OF THIS ASSERTED THE LINE WAS ABSENT, WHICH IS THE WRONG
# CRITERION AND WOULD HAVE FAILED ON A CORRECT BUILD. The report prints one row
# per layer and says what each resolved to; a layer that resolved to nothing is
# reported as nothing:
#
#     c-abi             —
#
# That is the report doing its job — a row that vanished would be
# indistinguishable from a row nobody looked at. What must not appear is a
# NAME, which is what the earlier draft of this file actually saw:
#
#     c-abi  picolibc-riscv (xim:picolibc-riscv@1.8.12, prebuilt)
c_abi_line="$(printf '%s\n' "$out" | grep -E '^\s*c-abi' | head -1)"
case "$c_abi_line" in
  *"—"*|"")
    echo "  ok  the c-abi layer names nothing — the target has no C library" ;;
  *)
    echo "FAIL: a C library was resolved for a target that has none"
    echo "        $c_abi_line"; exit 1 ;;
esac

bin="$(find target -type f -name okbare | head -1)"
[ -n "$bin" ] || { echo "FAIL: no artefact"; exit 1; }

desc="$(file -b "$bin")"
case "$desc" in
  *RISC-V*) echo "  ok  RISC-V" ;;
  *) echo "FAIL: wrong machine — $desc"; exit 1 ;;
esac

# ── It boots on a machine whose firmware provides the SBI ──────────────────
# ⚠️ THE PAYLOAD'S COPY FIRST, AND `command -v` ONLY AS A FALLBACK.
#
# On a machine with xlings there is a `qemu-system-riscv64` on PATH that is a
# shim, and asking it to run anything answers:
#
#     [error] qemu-system-riscv64 is not installed in this subos (_)
#
# `command -v` finds it and reports success, so a probe written PATH-first
# selects a program that cannot run and reports the failure as the test's.
# The payload's copy is the one this build system installed on purpose.
q="$(ls -d "$HOME"/.mcpp/registry/data/xpkgs/xim-x-qemu-riscv/*/bin/qemu-system-riscv64 2>/dev/null | head -1)"
if [ -z "$q" ] || ! "$q" --version > /dev/null 2>&1; then
    q="$(command -v qemu-system-riscv64 || true)"
    # And the fallback is checked the same way, for the same reason.
    [ -n "$q" ] && ! "$q" --version > /dev/null 2>&1 && q=""
fi
if [ -z "$q" ]; then
    echo "  SKIP  no riscv64 machine emulator here — linking is not booting"
else
    log="$work/run.log"
    # A watchdog: a program that never returns fails as surely as one that
    # returns wrongly, and without this the test would spend its whole timeout
    # discovering that.
    ( "$q" -machine virt -nographic -no-reboot -bios default -kernel "$bin" > "$log" 2>&1 ) & pid=$!
    ( sleep 40; kill -9 $pid 2>/dev/null ) & guard=$!
    wait $pid 2>/dev/null || true
    kill $guard 2>/dev/null || true

    # ⚠️ The emulated console ends its lines with CRLF, so a comparison that
    # does not strip them fails on a line that is correct.
    if tr -d '\r' < "$log" | grep -q 'okbare alive'; then
        echo "  ok  it boots and prints through openkal.stream"
    else
        echo "FAIL: the machine did not print what the program wrote"
        sed 's/^/        /' "$log" | head -8
        exit 1
    fi
fi

echo "OK: openkal runs on a machine with no operating system and no C library"
