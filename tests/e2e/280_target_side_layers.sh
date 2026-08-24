#!/usr/bin/env bash
# requires:
# The target side is five layers, and the report shows what is not ordinary.
#
# WHY THIS FILE EXISTS.
#
# A zero-configuration build resolves every layer from one compiler payload. A
# report that prints five lines reading `(payload)` answers a question nobody
# asked, and the lines that matter — a C library or a C++ runtime that came from
# somewhere else — are then read out of a block that always looks the same.
#
# The two assertions below are the two halves of that rule: silence when there
# is nothing to say, and every layer when the reader asks for it.
set -e

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

"$MCPP" new plain > /dev/null
cd plain

# ── 1. Nothing came from elsewhere, so no layer earns a line ────────────────
out="$("$MCPP" build 2>&1)"
target_line="$(printf '%s\n' "$out" | grep -E '^\s+Target ' || true)"
[[ -n "$target_line" ]] || { echo "no Target line at all:"; echo "$out"; exit 1; }

for label in compiler compiler-runtime kernel-abi c-abi c++-abi; do
    if printf '%s\n' "$out" | grep -qE "^\s+${label}\s"; then
        echo "a zero-configuration build printed the '$label' layer, which came"
        echo "from the compiler payload and is therefore not news:"
        echo "$out"
        exit 1
    fi
done

# ── 2. Asked for it, every layer is there ──────────────────────────────────
"$MCPP" clean > /dev/null 2>&1
verbose="$(MCPP_VERBOSE=1 "$MCPP" build 2>&1)"
for label in compiler compiler-runtime kernel-abi c-abi c++-abi; do
    printf '%s\n' "$verbose" | grep -qE "^\s+${label}\s" || {
        echo "MCPP_VERBOSE did not print the '$label' layer:"
        printf '%s\n' "$verbose" | grep -A 8 'Target '
        exit 1
    }
done

# The compiler layer reports the FAMILY, which is the spelling every toolchain
# spec and every capability uses. `clang` is the driver's name and would make a
# requirement written as `mcpp:compiler=llvm` unsatisfiable.
printf '%s\n' "$verbose" | grep -qE "^\s+compiler\s+(llvm|gcc|msvc)\s" || {
    echo "the compiler layer must report a family, not a driver name:"
    printf '%s\n' "$verbose" | grep -E '^\s+compiler\s'
    exit 1
}

echo "OK"
