#!/usr/bin/env bash
# requires: llvm unix-shell
# The same stack, for a machine this one is not.
#
# ⚠️ `aarch64-linux-musl` APPEARED IN NO e2e SCRIPT UNTIL THIS ONE. It is a
# `verified` row of the target table and the acceptance target named in
# mcpp-community/mcpp#492, and 278 scripts mentioned it zero times. What
# verified it was a hand-written probe run once — which says the target worked
# on the day someone looked, and nothing about tomorrow.
#
# ⭐ CROSSING IS WHERE A TARGET SIDE FROM THE GRAPH EARNS ITS KEEP, AND WHERE
# ITS MISTAKES ARE VISIBLE. A native build that quietly takes the payload's C
# library still runs; a cross that does produces an artefact for the wrong
# machine, and `file` says so in one line.
#
# ⚠️ AND THE HELPERS ARE ASSERTED, NOT JUST THE ARCHITECTURE. clang turns on
# `+outline-atomics` for aarch64 whenever the compiler runtime is compiler-rt,
# and the `__aarch64_*` helpers that feature calls live in compiler-rt rather
# than anywhere the engine could supply. A build that links without them is not
# possible; a build that links because the feature was switched OFF is, and
# reads identically from the outside. The LSE instruction count separates them.
set -e

MCPP="${MCPP:-mcpp}"
TARGET=aarch64-linux-musl
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app/src"
cd "$work/app"

cat > mcpp.toml <<'TOML'
[package]
name    = "okcross"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[dependencies]
openkal-musl = "0.3.5"
openkal-llvm-runtime = "0.1.3"
TOML

# `compare_exchange` and `fetch_add` are the two operations that reach the
# outline-atomics helpers on this architecture; everything else compiles to a
# single instruction and would prove nothing.
cat > src/main.cpp <<'CPP'
#include <atomic>
#include <cstdio>

int main() {
    std::atomic<int> a{7};
    int expected = 7;
    const bool swapped = a.compare_exchange_strong(expected, 42);
    const int before = a.fetch_add(5);
    std::printf("%d %d %d\n", swapped ? 1 : 0, before, a.load());
    return (swapped && before == 42 && a.load() == 47) ? 0 : 1;
}
CPP

if ! out="$("$MCPP" build --target "$TARGET" 2>&1)"; then
    case "$out" in
      *"not found in the synced index"*|*"install_packages failed"*)
        echo "SKIP: the openkal packages are not reachable from here"; exit 0 ;;
      *"cannot be built on this host"*)
        echo "SKIP: $TARGET cannot be built here"; exit 0 ;;
    esac
    echo "FAIL: the cross build failed"
    printf '%s\n' "$out" | grep -iE 'error' | head -5
    exit 1
fi

bin="$(find target -type f -name okcross | head -1)"
[ -n "$bin" ] || { echo "FAIL: no artefact"; exit 1; }

desc="$(file -b "$bin")"
case "$desc" in
  *"ARM aarch64"*) echo "  ok  ARM aarch64" ;;
  *) echo "FAIL: wrong machine — $desc"; exit 1 ;;
esac
case "$desc" in
  *"statically linked"*) echo "  ok  statically linked" ;;
  *) echo "FAIL: not static — $desc"; exit 1 ;;
esac

# ── The helpers are supplied, and the feature is on ─────────────────────────
nm="$(command -v llvm-nm || command -v nm || true)"
if [ -n "$nm" ]; then
    defined="$("$nm" "$bin" 2>/dev/null | grep -cE ' [TtWw] __aarch64_' || true)"
    undef="$("$nm" "$bin" 2>/dev/null | grep -cE ' U __aarch64_' || true)"
    if [ "${defined:-0}" -gt 0 ] && [ "${undef:-0}" = 0 ]; then
        echo "  ok  $defined outline-atomics helpers defined, 0 undefined"
    else
        echo "FAIL: ${defined:-0} defined, ${undef:-0} undefined — the graph did not supply them"
        "$nm" "$bin" 2>/dev/null | grep '__aarch64_' | head -4 | sed 's/^/        /'
        exit 1
    fi
fi

objdump="$(command -v llvm-objdump || command -v objdump || true)"
if [ -n "$objdump" ]; then
    lse="$("$objdump" -d "$bin" 2>/dev/null | grep -cE '\b(casal|cas|ldaddal|ldadd|swpal|swp)\b' || true)"
    if [ "${lse:-0}" -gt 0 ]; then
        echo "  ok  $lse LSE instructions — the feature is on, not switched off"
    else
        echo "FAIL: no LSE instruction; \`+outline-atomics\` looks disabled rather than supported"
        exit 1
    fi
fi

# ── And it runs, if this machine can run it ────────────────────────────────
runner="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
if [ -z "$runner" ]; then
    echo "  SKIP  no aarch64 emulator here — building is not running"
else
    if ran="$("$runner" "$bin" 2>&1)"; then
        case "$ran" in
          "1 42 47") echo "  ok  it runs under $(basename "$runner"): $ran" ;;
          *) echo "FAIL: wrong output under emulation: $ran"; exit 1 ;;
        esac
    else
        echo "FAIL: non-zero exit under $(basename "$runner"): $ran"
        exit 1
    fi
fi

echo "OK: the openkal stack crosses to aarch64, supplies its atomics helpers and runs"
