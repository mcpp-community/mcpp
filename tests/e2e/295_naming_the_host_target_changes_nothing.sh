#!/usr/bin/env bash
# requires: gcc unix-shell jq
# Spelling out the target this machine already builds for changes nothing.
#
# THIS IS AN IDENTITY, NOT A THRESHOLD. `mcpp build` and
# `mcpp build --target <the host's own target>` describe the same build for the
# same machine, so the link line either is the same string or something decided
# on the spelling rather than on the build.
#
# MEASURED 2026-08-26, ON THE MACHINE THIS WAS WRITTEN ON:
#
#     $ mcpp build                            → ELF 64-bit LSB pie executable
#     $ mcpp build --target x86_64-linux-gnu  → hermetic link check failed
#
# with llvm. The two link lines differed by five flags:
#
#     -stdlib=libc++  --rtlib=compiler-rt  --unwindlib=libunwind
#     -Wl,--push-state,--as-needed  -latomic
#
# THE CAUSE IS ONE CONDITION. `flags.cppm` asks `!crossTarget.empty()` — is
# there a `--target=` on the command line — and its own comment says what it
# meant to ask: "THE TARGET SIDE COMES FROM THE GRAPH". Those are different
# questions, and a project that names its host target while using no
# dependencies at all answers yes to the first and no to the second.
#
# NO EXPECTED VALUES. This asserts a relation between two runs, so it holds
# on every host and needs no table to compare against — which is why it is the
# first thing to land.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cd "$work"
printf '#include <cstdio>\nint main() { std::printf("ok\\n"); }\n' > src/main.cpp

# The target this host builds for when nothing is said. Taken from mcpp itself
# rather than assembled from `uname`: the point is to name the target mcpp would
# have chosen, and deriving it a second way would test the derivation instead.
# THE TARGET IS PER TOOLCHAIN, NOT PER MACHINE — AND THREE DRAFTS ASSUMED
# OTHERWISE.
#
# The identity is "naming the target this build would use anyway changes
# nothing". On Linux that is the machine's own target for every family, so
# `data.host` worked and hid the assumption. On Windows it does not: the host
# target is `x86_64-windows-msvc`, and a mingw gcc targets
# `x86_64-windows-gnu`. Measured on windows-2022:
#
#     FAIL: gcc@16.1.0 — one spelling built and the other did not
#             explicit: produced nothing
#
# — correct behaviour. gcc cannot emit `-msvc`, and the test had asked it to.
#
# So ask the query what THIS toolchain resolves to with no target named, and
# then name that. Exact on every host, and it needs no table.
implicit_target() {   # toolchain spec → the triple it would use anyway
    "$MCPP" why toolchain --toolchain "$1" --format json 2>/dev/null \
        | jq -r '.data.triple.toolchain // empty' | tr -d '\r'
}

# TWO LINES, AND FOR A LONG TIME THIS TEST READ ONLY ONE.
#
# The identity is about THE BUILD, and a build has a compile line as well as a
# link line. `2026.8.26.1` corrected the link side; the compile side kept
# asking `!crossTarget.empty()` and kept getting it wrong, and this test could
# not see that because it compared `^ldflags` alone.
#
# Measured on 2026.8.26.2 — same machine, same compiler, same target, differing
# only in whether it was spelled out — the compile line lost SIX tokens:
#
#     --no-default-config  -nostdinc++
#     -isystem <payload>/include/c++/v1
#     -isystem <payload>/include/<triple>/c++/v1
#     -isystem <glibc>/include
#     -isystem <linux-headers>/include
#
# ⇒ headers from one library and objects linked from another, silently, on any
# machine that happens to have system headers.
line_of() {   # channel, extra args… → that line of build.ninja, or nothing
    local channel="$1"; shift
    rm -rf target
    "$MCPP" build "$@" >/dev/null 2>&1 || true
    local f; f="$(find target -name build.ninja 2>/dev/null | head -1)"
    # AN EXPLICIT `return 0`, BECAUSE THIS FUNCTION IS ALLOWED TO FIND
    # NOTHING. The first draft ended on `[ -n "$f" ] && grep …`, whose exit
    # status under `set -e` is the test's — so the one case this test exists to
    # examine, a build that produced no link line, killed the script before it
    # could say so. It exited 1 with no output at all.
    if [ -n "$f" ]; then
        grep -m1 "^$channel" "$f" || true
    fi
    return 0
}

# `--target=` is expected on the explicit side and only there — it is the one
# token that names which machine. `-fprebuilt-module-path=` names the build
# directory, which differs because the fingerprint does; that is the mechanism
# working, not a difference in what is compiled.
#
# NOT ANCHORED WITH `^`, AND WINDOWS IS WHY. The ninja channel QUOTES a token
# whose path needs it, so the same flag arrives as
# `"-fprebuilt-module-path=C$:\Users\..."` — an anchored pattern misses it and
# the comparison then fails on the one token this filter exists to remove.
# Measured on windows-x86_64, where it turned a green invariant into a red one
# for a reason that had nothing to do with what was being compiled.
normalise() {
    printf '%s\n' "$1" | tr ' ' '\n' \
        | grep -v -- '--target=' | grep -v -- '-fprebuilt-module-path=' \
        | grep -v '^$' | sort
}

fail=0
checked=0

for tc in gcc llvm; do
    # THE TOOLCHAIN IS NAMED, because the defect lives on one of them and a
    # run that silently used the other would pass while proving nothing.
    # ONE FIELD, NOT A COLUMN. Earlier drafts read `$NF` — which is
    # `(default)` on exactly the row most likely to be picked — and then
    # `grep -oP`, which does not exist on macOS and would have turned this half
    # into a silent skip there.
    ver="$("$MCPP" toolchain list --format json 2>/dev/null \
           | jq -r --arg t "$tc" '[.data.toolchains[] | select(.family==$t) | .version][0] // empty' | tr -d '\r')"
    if [ -z "$ver" ]; then
        echo "  SKIP  $tc is not installed here"
        continue
    fi
    printf '[package]\nname    = "idprobe"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "%s@%s"\n' \
        "$tc" "$ver" > mcpp.toml

    ht="$(implicit_target "$tc@$ver")"
    if [ -z "$ht" ]; then
        echo "  SKIP  the query did not name a target for $tc@$ver"
        continue
    fi

    implicit="$(line_of ldflags)"
    explicit="$(line_of ldflags --target "$ht")"
    implicit_cxx="$(line_of cxxflags)"
    explicit_cxx="$(line_of cxxflags --target "$ht")"

    if [ -z "$implicit" ] || [ -z "$explicit" ]; then
        # EARNED, NOT ASSUMED. One of the two produced no build.ninja at all,
        # which is itself the asymmetry this test is about — so it is only a
        # skip when NEITHER produced one.
        if [ -z "$implicit" ] && [ -z "$explicit" ]; then
            echo "  SKIP  $tc@$ver built nothing either way here"
            continue
        fi
        echo "FAIL: $tc@$ver — one spelling built and the other did not"
        echo "        implicit: ${implicit:+produced a link line}${implicit:-produced nothing}"
        echo "        explicit: ${explicit:+produced a link line}${explicit:-produced nothing}"
        fail=1
        checked=$((checked+1))
        continue
    fi

    checked=$((checked+1))

    for channel in ldflags cxxflags; do
        if [ "$channel" = ldflags ]; then
            lhs="$implicit"; rhs="$explicit"
        else
            lhs="$implicit_cxx"; rhs="$explicit_cxx"
        fi
        # BOTH SIDES MUST HAVE CONTENT. Two empty strings compare equal, and
        # a comparison that passes on nothing is the false green this file's
        # other guard already exists for.
        if [ -z "$lhs" ] || [ -z "$rhs" ]; then
            echo "FAIL: $tc@$ver: no $channel line to compare"
            fail=1
            continue
        fi
        a="$(normalise "$lhs")"
        b="$(normalise "$rhs")"
        if [ "$a" = "$b" ]; then
            echo "  ok  $tc@$ver: naming $ht changes nothing ($channel)"
        else
            echo "FAIL: $tc@$ver: naming $ht changed the $channel line"
            diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") \
                | grep -E '^[<>]' | head -8 | sed 's/^/        /'
            fail=1
        fi
    done
done

if [ "$checked" = 0 ]; then
    echo "SKIP: no toolchain here produced a link line to compare"
    exit 0
fi
[ "$fail" = 0 ] || exit 1
echo "OK: naming the host's own target changes nothing ($checked toolchains)"
