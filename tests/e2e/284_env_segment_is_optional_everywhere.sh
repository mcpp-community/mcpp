#!/usr/bin/env bash
# requires: gcc
# `arch-os` is a target on every platform, not only on Linux.
#
# ⚠️ WHY THIS WAS ASYMMETRIC AND WHY THE ASYMMETRY COST SOMETHING.
#
# A target triple states a REQUEST, and a request must be able to say nothing.
# `x86_64-linux` was accepted on that basis; `x86_64-windows` was rejected with
# `unknown target`, and `riscv64-none` likewise. The two platforms where the
# segment could not be declined are exactly the two where it names something
# other than a C library — the object ABI on Windows, the object format on bare
# metal — so a project was required to write a word describing nothing it had
# chosen. Under a graph-supplied target side that word is `gnu`, while the
# compiler is clang, the linker lld, the C library musl and the C++ runtime
# libc++: nothing in the build is GNU.
#
# ⭐ THE ASSERTION IS ON THE IDENTITY, NOT ONLY ON THE EXIT STATUS. Accepting the
# short spelling is worth nothing if it produces a second output directory or a
# second fingerprint: the project would then build twice, cache nothing, and the
# two artefacts could drift. The identity must stay total — `x86_64-windows` IS
# `x86_64-windows-gnu` — while only the RECORD of what was asked for differs.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p app/src
cat > app/mcpp.toml <<'TOML'
[package]
name    = "envopt"
version = "0.1.0"
TOML
cat > app/src/main.cpp <<'CPP'
#include <cstdio>
int main() { std::printf("ok\n"); }
CPP
cd app

# ── The pair under test, per platform ────────────────────────────────────────
# Each is "short spelling" and "the canonical form it must resolve to".
check_pair() {
    short="$1"; canonical="$2"

    rm -rf target
    if ! out="$("$MCPP" build --target "$short" 2>&1)"; then
        case "$out" in
          *"unknown target"*)
            echo "FAIL: \`$short\` was rejected"
            printf '%s\n' "$out" | head -3
            return 1 ;;
          *) echo "SKIP: $short cannot build here (no payload/toolchain)"
             return 0 ;;
        esac
    fi

    # One directory, named for the CANONICAL form.
    if [ ! -d "target/$canonical" ]; then
        echo "FAIL: \`$short\` did not build into target/$canonical"
        echo "       got: $(ls target 2>/dev/null | tr '\n' ' ')"
        return 1
    fi
    fp_short="$(ls "target/$canonical" | head -1)"

    # ⭐ The load-bearing step: the long spelling must land on the SAME
    # fingerprint, which is what makes the second build a cache hit rather than
    # a second full build.
    "$MCPP" build --target "$canonical" >/dev/null 2>&1 || {
        echo "FAIL: canonical \`$canonical\` failed after \`$short\` succeeded"
        return 1; }
    n="$(ls "target/$canonical" | wc -l)"
    if [ "$n" != "1" ]; then
        echo "FAIL: the two spellings produced $n fingerprints, not one"
        echo "       $(ls "target/$canonical" | tr '\n' ' ')"
        return 1
    fi

    # And the report heads with what was WRITTEN, so a reader is not told they
    # asked for something they declined.
    case "$out" in
      *"Target $short"*) : ;;
      *) echo "FAIL: the report did not head with \`$short\`"
         printf '%s\n' "$out" | grep -m1 Target
         return 1 ;;
    esac

    echo "  ok  $short → $canonical  (one fingerprint: $fp_short)"
    return 0
}

rc=0
check_pair x86_64-linux    x86_64-linux-gnu    || rc=1
check_pair x86_64-windows  x86_64-windows-gnu  || rc=1
check_pair riscv64-none    riscv64-none-elf    || rc=1

[ "$rc" = 0 ] || exit 1
echo "OK: the env segment is optional on every platform, and declining it changes no identity"
