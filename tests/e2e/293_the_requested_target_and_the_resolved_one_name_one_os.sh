#!/usr/bin/env bash
# requires: gcc unix-shell
# A build for one operating system is never quietly performed for another.
#
# ⚠️ MEASURED 2026-08-25 IN CI. On a machine that had installed only a native
# gcc, the cross payload was absent, resolution fell back to the host compiler,
# and the report said so on one line while the build carried on:
#
#     Target x86_64-windows-gnu → x86_64-unknown-linux-gnu
#     …
#     src/stream.cpp:68:9: error: 'GetFileType' was not declared in this scope
#
# Two operating systems, one line, no diagnostic. Windows sources were compiled
# for Linux and the failure surfaced a hundred lines later naming a Win32
# function — a symbol, not the decision that produced it. openkal-uefi reached
# the same fallback at the linker: `ld: unrecognized option '--subsystem'`.
#
# ⭐⭐ BOTH DIRECTIONS, BECAUSE A REFUSAL THAT FIRES TOO OFTEN IS WORSE THAN THE
# DEFECT. `x86_64-windows-gnu → x86_64-w64-windows-gnu` differs in vendor and
# spelling and is exactly right; refusing on anything but the OS would reject
# every correct cross build. The second half is a sweep that must not refuse.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

make_project() {   # dir target-section
    mkdir -p "$1/src"
    { printf '[package]\nname    = "osprobe"\nversion = "0.1.0"\n'
      [ -n "$2" ] && printf '\n%s\n' "$2"; } > "$1/mcpp.toml"
    printf '#include <cstdio>\nint main() { std::printf("ok\\n"); }\n' > "$1/src/main.cpp"
}

# ── Half one: it refuses, and says which two systems ──────────────────────
#
# `toolchain = "system"` hands the build the PATH compiler; pointing it at a
# Windows target is the one arrangement that reproduces CI's fallback without
# uninstalling anything.
#
# ⚠️ AND SINCE THE SYSTEM TOOLCHAIN IS REFUSED, THAT REFUSAL IS ALSO AN ANSWER
# TO THIS TEST'S QUESTION. mcpp builds only with toolchains it manages
# (`msvc@system` excepted), and the refusal fires before target resolution — so
# a Windows target can no longer reach a Linux host compiler through this door
# at all. The invariant holds by a stronger mechanism than the one this test was
# written against, and BOTH refusals count. What must never happen, and what the
# else-branch below still fails on, is the build going ahead.
make_project "$work/mismatch" '[target.x86_64-windows-gnu]
toolchain = "system"'
out="$(cd "$work/mismatch" && "$MCPP" build --target x86_64-windows-gnu 2>&1 || true)"

# ⚠️⚠️ AND A SKIP HERE HAS TO BE EARNED, OR THE TEST CANNOT SEE A REVERT.
# The first draft took the skip branch whenever no refusal appeared — which is
# precisely what the unfixed build does, so reverting the fix turned this test
# green-by-silence rather than red. The arrangement either reproduced (the
# report names two systems) or it did not; only the second is a skip.
reported="$(printf '%s\n' "$out" | grep -oP 'Target \K\S+ → \S+' | head -1)"
half_one_done=0
case "$out" in
  *"different operating systems"*)
    echo "  ok  it refuses rather than building for the wrong system" ;;
  *"is not supported: mcpp builds only with toolchains it manages"*)
    # The stronger refusal: the arrangement cannot be expressed any more, so a
    # Windows target never reaches a Linux compiler through this door.
    #
    # ⚠️ AND IT MUST NOT `exit 0` HERE. Half two is independent of half one and
    # asserts that correct cross builds still go through; leaving early skips
    # it, and the ecosystem job that runs this file checks that each test "ran
    # to its conclusion" precisely so an early exit cannot masquerade as a
    # pass. Caught by that job, not by a local run.
    echo "  ok  the system toolchain is refused outright, so the mismatch"
    echo "      this test guards cannot be reached through it"
    half_one_done=1 ;;
  *)
    asked="${reported%% → *}"
    resolved="${reported##* → }"
    if [ -n "$reported" ] \
       && printf '%s' "$asked"    | grep -q 'windows' \
       && printf '%s' "$resolved" | grep -q 'linux'; then
        echo "FAIL: a Windows target resolved to a Linux toolchain and the build went on"
        echo "        $reported"
        # ⚠️ THE WHOLE OUTPUT, because the one line above says WHAT happened and
        # not which decision produced it. This failure first appeared only in
        # CI, where the arrangement differs from any machine it was written on,
        # and a one-line report cannot be read backwards into a cause.
        echo "        ── what mcpp said ──"
        printf '%s\n' "$out" | sed 's/^/        /'
        exit 1
    fi
    echo "SKIP: this host did not reproduce the fallback"
    printf '%s\n' "$out" | grep -iE 'Target |error' | head -3 | sed 's/^/        /'
    exit 0 ;;
esac

# ⭐ AND THE MESSAGE NAMES BOTH. A refusal that does not say what it resolved
# to leaves the reader with the same question the report used to answer.
#
# Only asked of the OS-mismatch refusal. The toolchain refusal is a different
# sentence about a different decision — it never resolved a target at all —
# and demanding both triples from it would be asserting on the wrong object.
if [ "$half_one_done" = 0 ]; then
    ok=1
    printf '%s\n' "$out" | grep -q "x86_64-windows-gnu" || ok=0
    printf '%s\n' "$out" | grep -q "linux"              || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok  and it names the target asked for and the one resolved"
    else
        echo "FAIL: the refusal does not name both systems"
        printf '%s\n' "$out" | head -4 | sed 's/^/        /'
        exit 1
    fi
fi

# ── Half two: every correct cross build still goes through ────────────────
#
# ⚠️ A sweep that swept nothing is a SKIP, not a pass.
built=0; skipped=0
for t in x86_64-linux-gnu x86_64-linux-musl aarch64-linux-musl x86_64-windows-gnu; do
    make_project "$work/ok-$t" ""
    o="$(cd "$work/ok-$t" && "$MCPP" build --target "$t" 2>&1 || true)"
    case "$o" in
      *"different operating systems"*)
        echo "FAIL: a correct cross build was refused: $t"
        printf '%s\n' "$o" | grep -i 'Target ' | head -1 | sed 's/^/        /'
        exit 1 ;;
      *"Target "*)
        line="$(printf '%s\n' "$o" | grep -oP 'Target \K\S+ → \S+' | head -1)"
        echo "  ok    $line"
        built=$((built+1)) ;;
      *)
        skipped=$((skipped+1)) ;;
    esac
done

if [ "$built" = 0 ]; then
    echo "SKIP: no target resolved here — the sweep proved nothing ($skipped skipped)"
    exit 0
fi
echo "  ok  $built correct cross targets went through untouched"

echo "OK: the requested target and the resolved one name one operating system"
