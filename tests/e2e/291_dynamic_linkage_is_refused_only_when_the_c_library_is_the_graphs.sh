#!/usr/bin/env bash
# requires: gcc elf network
# `linkage = "dynamic"` is reported as ineffective only when it actually is.
#
# ⚠️ THE PREDICATE USED TO SPAN TWO LAYERS AND THE REASON SPANS ONE. The
# warning's own justification — "those packages are compiled into this build as
# objects, and there is no shared object to link against" — is a property of
# the C LIBRARY. A backend that runs ON a platform takes its kernel interface
# from the graph and keeps the payload's C library, and a payload libc has a
# shared object, so `dynamic` is honoured there.
#
# Measured 2026-08-25 before the fix, on the project this file builds:
#
#     warning: `linkage = "dynamic"` has no effect … The artifact is static.
#     $ file …/dynprobe   → dynamically linked
#     $ readelf -d …      → NEEDED libm.so.6, libgcc_s.so.1, libc.so.6
#
# ⭐⭐ BOTH DIRECTIONS, BECAUSE ONLY ONE OF THEM IS THE FIX. Deleting the
# warning outright also stops it lying, and that would lose the diagnostic the
# directive needs when the C library really does come from the graph — which is
# why the second half builds that arrangement and requires the warning to
# appear.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ⚠️ AN EXPLICIT `--target` IS LOAD-BEARING. `[target.<triple>]` applies to the
# target that was REQUESTED; a bare `mcpp build` requests none, the row never
# applies, `linkage` stays empty, and both halves of this test would pass
# without exercising anything.
TARGET=x86_64-linux-gnu

make_project() {
    local dir="$1" tc="$2" deps="$3"
    mkdir -p "$dir/src"
    cat > "$dir/mcpp.toml" <<TOML
[package]
name    = "linkprobe"
version = "0.1.0"

[toolchain]
default = "$tc"

[target.$TARGET]
linkage = "dynamic"

[dependencies]
$deps
TOML
    printf '#include <cstdio>\nint main() { std::printf("ok\\n"); }\n' \
        > "$dir/src/main.cpp"
}

warned() { printf '%s\n' "$1" | grep -q 'has no effect'; }

# ── Half one: kernel interface from the graph, C library from the payload ──
make_project "$work/onplatform" "gcc@16.1.0" 'openkal-linux = "0.5.4"'
out="$(cd "$work/onplatform" && "$MCPP" build --target "$TARGET" 2>&1)" || {
    echo "SKIP: the on-platform project did not build here"
    printf '%s\n' "$out" | grep -iE '^.*error.*$' | head -3
    exit 0
}

# The arrangement has to be the one this is about, or the assertion below is
# about nothing.
case "$out" in
  *kernel-abi*graph*) ;;
  *) echo "SKIP: the kernel interface did not come from the graph here"
     printf '%s\n' "$out" | grep -E 'abi' | sed 's/^/        /'
     exit 0 ;;
esac

bin="$(find "$work/onplatform/target" -name linkprobe -type f -perm -u+x | head -1)"
[ -n "$bin" ] || { echo "FAIL: no artifact was produced"; exit 1; }

if warned "$out"; then
    echo "FAIL: 'dynamic' was reported as ineffective while the payload's C library was in use"
    printf '%s\n' "$out" | grep 'has no effect' | sed 's/^/        /'
    exit 1
fi
echo "  ok  no warning when the C library is the payload's"

# ⭐ AND THE ARTIFACT AGREES. The warning's claim is "The artifact is static";
# a test that only checked for the absence of the text would pass on a build
# that silently produced a static binary anyway.
needed="$(readelf -d "$bin" 2>/dev/null | grep -c NEEDED || true)"
if [ "${needed:-0}" -gt 0 ]; then
    echo "  ok  'dynamic' was honoured — $needed DT_NEEDED entries"
else
    echo "FAIL: the artifact is static, so the warning would have been right"
    exit 1
fi

# ── Half two: the C library itself comes from the graph ────────────────────
# The same stack e2e 286 builds. openkal-llvm-runtime IS libc++/libc++abi/
# libunwind, so the toolchain has to be the one that package exists for — and
# it is the C library coming from the graph, not the C++ runtime, that this
# half is about.
make_project "$work/fullgraph" "llvm@22.1.8" 'openkal-musl = "0.3.5"
openkal-llvm-runtime = "0.1.3"'
out2="$(cd "$work/fullgraph" && "$MCPP" build --target "$TARGET" 2>&1)" || true

case "$out2" in
  *c-abi*graph*) ;;
  *) echo "SKIP: the C library did not come from the graph here"
     printf '%s\n' "$out2" | grep -E 'abi' | sed 's/^/        /'
     exit 0 ;;
esac

if warned "$out2"; then
    echo "  ok  the warning still appears when the C library is the graph's"
else
    echo "FAIL: 'dynamic' cannot be honoured here and nothing said so"
    exit 1
fi

echo "OK: the C library decides whether 'dynamic' can be honoured"
