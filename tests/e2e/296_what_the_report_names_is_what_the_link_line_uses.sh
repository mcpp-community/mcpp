#!/usr/bin/env bash
# requires: gcc unix-shell jq
# The layer the report names is the layer the link line reaches for.
#
# ⭐⭐ TWO RELATIONS, NO EXPECTED VALUES. Like e2e 295, this compares two things
# mcpp itself produced rather than checking them against a table, so it holds on
# every host and needs nothing installed beyond a working toolchain:
#
#   c-abi (payload)  ⇒  the link line must reach into that payload
#   c-abi (graph)    ⇒  the link line must NOT reach into this host's C library
#
# ⚠️ THE FIRST ONE IS THE SHAPE THIS RELEASE KEEPS PAYING FOR. `xim:glibc` is
# installed, carries Scrt1.o/crti.o/crtn.o, and the report says
# `c-abi glibc (payload)` — while llvm's link line contained no reference to it
# at all and the build failed on startup objects resolved from /lib. The report
# and the link line disagreed, and only the report was read.
#
# ⚠️ AND THE SECOND ONE IS ITS MIRROR. A graph-supplied C library that still
# carried this host's loader produced, measured 2026-08-23:
#
#     ld64.lld: error: unknown argument
#       '--dynamic-linker=…/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2'
#
# — accurate, and naming nothing about the decision. Both directions are here
# because a fix for either one alone can break the other.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cd "$work"
printf '#include <cstdio>\nint main() { std::printf("ok\\n"); }\n' > src/main.cpp

report=""; ldflags=""
build_and_read() {   # extra args… → sets $report and $ldflags
    rm -rf target
    report="$("$MCPP" build "$@" 2>&1 || true)"
    local f; f="$(find target -name build.ninja 2>/dev/null | head -1)"
    ldflags=""
    [ -n "$f" ] && ldflags="$(grep -m1 '^ldflags' "$f" || true)"
}

# The `c-abi` row's origin and the package it names, from the report.
c_abi_line() { printf '%s\n' "$report" | grep -E '^\s+c-abi\s' | head -1; }

fail=0; checked=0

# ── Relation one: a payload C library must be on the link line ─────────────
for tc in gcc llvm; do
    # ⭐ From the machine interface: `grep -oP` does not exist on macOS, and its
    # failure mode here was a skip rather than a red.
    ver="$("$MCPP" toolchain list --format json 2>/dev/null \
           | jq -r --arg t "$tc" '[.data.toolchains[] | select(.family==$t) | .version][0] // empty')"
    [ -n "$ver" ] || { echo "  SKIP  $tc is not installed here"; continue; }
    printf '[package]\nname    = "linkprobe"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "%s@%s"\n' \
        "$tc" "$ver" > mcpp.toml
    build_and_read --verbose
    line="$(c_abi_line)"
    case "$line" in
      *"(payload)"*) ;;
      *) echo "  SKIP  $tc@$ver: c-abi is not the payload's here (${line:-no row})"; continue ;;
    esac
    [ -n "$ldflags" ] || { echo "  SKIP  $tc@$ver produced no link line"; continue; }
    checked=$((checked+1))

    # ⭐⭐ THE QUERY NAMES THE DIRECTORY, AND THE LINK LINE MUST CONTAIN IT.
    #
    # `cLibrary.path` is what `resolve_link_model` decided — the same function
    # the flag emitter calls — so this asserts that what mcpp SAYS it will pass
    # is what mcpp passes. Nothing is hardcoded and nothing is per-platform.
    #
    # ⚠️ THE FIRST VERSION HARDCODED `registry/data/xpkgs` AND WAS WRONG ON
    # macOS. Measured on macos-14:
    #
    #     c-abi     libSystem   (payload)
    #     ldflags = -isysroot /Applications/Xcode_15.4.app/…/MacOSX.sdk
    #
    # — and that is CORRECT. Darwin's C library is the SDK's libSystem, and the
    # SDK belongs to the machine, not to a payload. The test was asserting a
    # Linux arrangement and calling its absence a defect.
    want="$("$MCPP" why toolchain --toolchain "$tc@$ver" --format json 2>/dev/null \
            | jq -r '.data.cLibrary.path // ""')"
    if [ -z "$want" ]; then
        # ⭐⭐ NO PATH IS ALSO A CLAIM, AND IT IS CHECKABLE. The query says mcpp
        # passes no C-library path — the self-contained arrangement, where the
        # driver carries its own (mingw g++, MSVC). The assertion is then the
        # other direction: nothing from OUTSIDE mcpp's store may appear either.
        #
        # ⚠️ THE FIRST VERSION SKIPPED HERE, and on windows-2022 both toolchains
        # took that branch — so relation one had no coverage on that host at all
        # and the test did not reach its conclusion.
        # ⚠️⚠️ THE FLAG AND ITS PATH ARE JOINED FIRST, AND BOTH SPELLINGS EXIST.
        #
        # `-B/usr/lib` is one word and `-B /usr/lib` is two. Splitting on spaces
        # and matching `^-B` flags the bare `-B` of the second form as a leak
        # with no path in it; requiring a path in the token instead makes the
        # second form invisible. Measured both, writing this line:
        #
        #     -B /usr/lib/gcc   → [-B]                      (false positive)
        #     -B /usr/lib/gcc   → []                        (false negative)
        #
        # ⭐ A false negative is the worse one here — this branch exists to
        # catch a link line reaching outside mcpp's store — so the two forms are
        # made one before anything is decided.
        leak="$(printf '%s\n' "$ldflags" \
                | sed -E 's/(--sysroot|-isysroot|-B|-L)[[:space:]]+/\1/g' \
                | tr ' ' '\n' \
                | grep -E '^(--sysroot=?|-isysroot=?|-B|-L)[^ ]*[/\\][^ ]*$' \
                | grep -vF 'registry' | head -3)"
        if [ -z "$leak" ]; then
            echo "  ok  $tc@$ver: a self-contained driver brings in nothing external"
        else
            echo "FAIL: $tc@$ver: the query names no C library, and the link line reaches outside"
            printf '        %s\n' $leak
            fail=1
        fi
        continue
    fi
    if printf '%s\n' "$ldflags" | grep -qF -- "$want"; then
        echo "  ok  $tc@$ver: the C library the query names is on the link line"
    else
        echo "FAIL: $tc@$ver: the query names a C library the link line does not use"
        echo "        query:   $want"
        printf '        ldflags: %s\n' "$(printf '%s' "$ldflags" | cut -c1-110)"
        fail=1
    fi
done

# ── Relation two: a graph C library must not drag the host's in ───────────
cat > mcpp.toml <<'TOML'
[package]
name    = "linkprobe"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[dependencies]
openkal-musl = "0.3.5"
openkal-llvm-runtime = "0.1.3"
TOML
printf 'import std;\nint main(){ std::println("ok"); }\n' > src/main.cpp
build_and_read
line="$(c_abi_line)"
case "$line" in
  *graph*)
    [ -n "$ldflags" ] || { echo "  SKIP  the graph build produced no link line"; :; }
    if [ -n "$ldflags" ]; then
        checked=$((checked+1))
        # This host's C library, by the two spellings mcpp itself would emit.
        if printf '%s\n' "$ldflags" | grep -qE 'xim-x-glibc|/lib/x86_64-linux-gnu|/usr/lib/gcc'; then
            echo "FAIL: c-abi comes from the graph, yet the link line reaches for this host's C library"
            printf '%s\n' "$ldflags" | tr ' ' '\n' \
                | grep -E 'xim-x-glibc|/lib/x86_64-linux-gnu|/usr/lib/gcc' | head -4 | sed 's/^/        /'
            fail=1
        else
            echo "  ok  a graph C library does not drag this host's in"
        fi
    fi ;;
  *) echo "  SKIP  the graph did not supply the C library here (${line:-no row})" ;;
esac

if [ "$checked" = 0 ]; then
    echo "SKIP: nothing here produced a link line to relate to a report"
    exit 0
fi
[ "$fail" = 0 ] || exit 1
echo "OK: what the report names is what the link line uses ($checked relations)"
