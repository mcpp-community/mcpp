#!/usr/bin/env bash
# requires: llvm unix-shell jq
# Naming your own compiler for a pinned target is allowed. Naming it and
# supplying nothing in place of what the pin supplied is not.
#
# ⭐⭐ THE TWO CASES ARE THE SAME MANIFEST MINUS ONE LINE.
#
#   [toolchain] default = "llvm@22.1.8"          → refused
#
#   [dependencies] openkal-llvm-runtime = "…"    → built
#   [toolchain]    default = "llvm@22.1.8"
#
# A hosted row's pin says "this payload supplies the target's C library". The
# escape hatch exists because a project whose graph supplies one instead has no
# use for it — which is `examples/06-openkal-cross`. With neither, clang is a
# retargetable compiler holding no C library at all.
#
# ⚠️ MEASURED 2026-08-26, before this file existed. Both spellings ran the whole
# build and failed at the link:
#
#     --target x86_64-linux-musl
#       hermetic link check failed … crtbeginT.o (bare name)
#     --target x86_64-windows-gnu
#       hermetic link check failed …
#       /usr/lib/gcc/x86_64-w64-mingw32/13-win32/crtbegin.o (outside the sandbox)
#
# Accurate about the symptom, silent about the decision — and the second one
# names a directory belonging to the HOST, on a machine that happened to have a
# system mingw. A machine without one fails differently, which is the other
# reason the answer must not come from the link.
#
# ⚠️⚠️ HALF TWO IS NOT DECORATION. A guard that refused every declared toolchain
# would pass half one and take away the escape hatch — and the openkal ecosystem
# is built entirely on it.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cd "$work"
printf '#include <cstdio>\nint main() { std::printf("ok\\n"); }\n' > src/main.cpp

llvmver="$("$MCPP" toolchain list --format json 2>/dev/null \
           | jq -r '[.data.toolchains[] | select(.family=="llvm") | .version][0] // empty')"
if [ -z "$llvmver" ]; then
    echo "SKIP: llvm is not installed here, and this test is about declaring it"
    exit 0
fi

fail=0

# ── Half one: a pinned row, a different family, and nothing in the graph ──
#
# The target is read from the machine rather than hardcoded: any row whose pin
# names a family other than llvm will do, and which rows exist is the target
# table's business, not this test's.
# ⭐⭐ `pin`, NOT `toolchain` — AND THE DIFFERENCE COST A RED CI RUN.
#
# `toolchain` is what the row is associated with: the installed payload on an
# installed row, the convention on a vocabulary row. A row can have the first
# and no second. This test needs a row that HAS a convention, so it reads the
# field that carries only that.
#
# Measured 2026-08-26 on ubuntu-24.04: selecting on `toolchain` picked
# `x86_64-linux-gnu`, whose convention pin is empty, and the test then demanded
# a refusal that correctly did not happen.
#
# The status filter matters for the same reason in the other direction: a
# `planned` row refuses under a different rule, and half one would pass while
# testing nothing.
pinned="$("$MCPP" toolchain list --format json 2>/dev/null \
          | jq -r '[.data.targets[]
                    | select(.pin | startswith("gcc"))
                    | select(.status != "planned")
                    | .target][0] // empty')"
if [ -z "$pinned" ]; then
    echo "SKIP: no target row here pins a non-llvm toolchain"
    exit 0
fi

printf '[package]\nname    = "convprobe"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "llvm@%s"\n' \
    "$llvmver" > mcpp.toml
rm -rf target
out="$("$MCPP" build --target "$pinned" 2>&1 || true)"

# ⭐⭐ CLASSIFY FROM THE MACHINE INTERFACE, ASSERT THE WORDING FROM THE MESSAGE.
# `data.reason` is a finite token and survives any rewording of the sentence;
# the sentence is still checked below, because naming the target, the convention
# and the replacement is a promise no code can keep on its own.
reason="$("$MCPP" why toolchain --target "$pinned" --toolchain "llvm@$llvmver" \
            --format json 2>/dev/null | jq -r '.data.reason // "-"')"

case "$reason" in
  convention-unreplaced)
    echo "  ok  overriding a convention with nothing in its place is refused" ;;
  none)
    # ⚠️ TWO OUTCOMES SHARE THIS BRANCH AND ONLY ONE IS A REGRESSION. If the
    # build also succeeded, an llvm payload now supplies this target's C library
    # and the refusal has outlived its reason — the table changed and this test
    # is the thing that says so. If it failed, the old behaviour is back: the
    # decision was let through and the link paid for it.
    case "$out" in
      *"Finished"*)
        echo "FAIL: $pinned linked with llvm alone — the refusal has outlived its reason" ;;
      *)
        echo "FAIL: the build ran and failed at the link instead of at the decision"
        printf '%s\n' "$out" | grep -iE 'error|crtbegin' | head -3 | sed 's/^/        /' ;;
    esac
    fail=1 ;;
  *)
    echo "SKIP: $pinned refused under '$reason', which is not the rule this test is about"
    exit 0 ;;
esac

# ⭐ AND IT POINTS AT THE WAY OUT. A refusal that names no replacement leaves
# the reader exactly where `crtbeginT.o (bare name)` left them.
if [ "$fail" = 0 ]; then
    ok=1
    printf '%s\n' "$out" | grep -q "$pinned"          || ok=0
    printf '%s\n' "$out" | grep -q 'openkal'          || ok=0
    printf '%s\n' "$out" | grep -q '\[toolchain\]'    || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok  and it names the target, the convention and the replacement"
    else
        echo "FAIL: the refusal does not point at the way out"
        printf '%s\n' "$out" | head -8 | sed 's/^/        /'
        fail=1
    fi
fi

# ── Half two: the same declaration, with a graph that supplies the C library ──
#
# ⚠️ THE DEPENDENCY IS THE ONLY DIFFERENCE. Same compiler, same target, same
# source.
printf '[package]\nname    = "convprobe"\nversion = "0.1.0"\n\n[dependencies]\nopenkal-llvm-runtime = "0.1.3"\n\n[toolchain]\ndefault = "llvm@%s"\n' \
    "$llvmver" > mcpp.toml
printf 'import std;\nint main() { std::println("ok"); }\n' > src/main.cpp
rm -rf target
graph="$("$MCPP" build --target "$pinned" --verbose 2>&1 || true)"

# ⭐⭐ THE CLAIM IS THAT THE GUARD STOOD ASIDE, NOT THAT openkal COMPILES ON THIS
# ARCHITECTURE — AND THE FIRST VERSION ASSERTED THE SECOND.
#
# It required `Finished`, so a dependency failing for its own reasons turned
# half two into a skip and left the guard's escape hatch untested. Measured
# 2026-08-26 on `aarch64-linux-musl`:
#
#     …/compiler-rt/lib/builtins/truncxfhf2.c:13:36:
#       error: unknown type name 'xf_float'; did you mean 'tf_float'?
#
# — an x87 long-double builtin on a machine with no x87. Nothing to do with
# whether a declared toolchain was honoured, and a test that cannot tell the
# difference reports on whichever it happened to hit.
#
# The two things that ARE the claim: the refusal did not fire, and the graph
# really did supply the C library (otherwise "not refused" is vacuous — an
# unresolved dependency would satisfy it too).
graphReason="$("$MCPP" why toolchain --target "$pinned" --toolchain "llvm@$llvmver" \
                 --format json 2>/dev/null | jq -r '.data.reason // "-"')"
graphCabi="$("$MCPP" why toolchain --target "$pinned" --toolchain "llvm@$llvmver" \
               --format json 2>/dev/null \
             | jq -r '[.data.layers[] | select(.layer=="c-abi") | .origin][0] // "-"')"

if [ "$graphReason" = convention-unreplaced ]; then
    echo "FAIL: a graph-supplied C library was refused — the escape hatch is gone"
    printf '%s\n' "$graph" | head -4 | sed 's/^/        /'
    fail=1
elif [ "$graphCabi" = graph ]; then
    echo "  ok  the same declaration is honoured when the graph supplies the C library"
else
    # ⚠️ NOT A PASS, AND NOT A SKIP. "Not refused" on its own is satisfied by a
    # dependency that never resolved, so the second condition is what makes the
    # first one mean anything.
    echo "FAIL: c-abi came from '$graphCabi', not the graph — half two proves nothing"
    printf '%s\n' "$graph" | grep -iE 'error' | head -3 | sed 's/^/        /'
    fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "OK: a convention may be overridden, but not merely removed"
