#!/usr/bin/env bash
# requires: gcc unix-shell jq
# Spelling out the target this machine already builds for changes nothing.
#
# ⭐⭐ THIS IS AN IDENTITY, NOT A THRESHOLD. `mcpp build` and
# `mcpp build --target <the host's own target>` describe the same build for the
# same machine, so the link line either is the same string or something decided
# on the spelling rather than on the build.
#
# ⚠️ MEASURED 2026-08-26, ON THE MACHINE THIS WAS WRITTEN ON:
#
#     $ mcpp build                            → ELF 64-bit LSB pie executable
#     $ mcpp build --target x86_64-linux-gnu  → hermetic link check failed
#
# with llvm. The two link lines differed by five flags:
#
#     -stdlib=libc++  --rtlib=compiler-rt  --unwindlib=libunwind
#     -Wl,--push-state,--as-needed  -latomic
#
# ⭐ THE CAUSE IS ONE CONDITION. `flags.cppm` asks `!crossTarget.empty()` — is
# there a `--target=` on the command line — and its own comment says what it
# meant to ask: "THE TARGET SIDE COMES FROM THE GRAPH". Those are different
# questions, and a project that names its host target while using no
# dependencies at all answers yes to the first and no to the second.
#
# ⭐ NO EXPECTED VALUES. This asserts a relation between two runs, so it holds
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
host_target() {
    # ⭐ THE MACHINE INTERFACE NAMES IT DIRECTLY. `data.host` is the host
    # triple, which is what "this machine's own target" means — no column to
    # locate and no note to recognise.
    #
    # ⚠️ THE FIRST TWO DRAFTS BOTH READ THE HUMAN TABLE AND BOTH READ IT WRONG.
    # One took the `*`, which marks the default toolchain PAIR and on this
    # machine sat on `aarch64-linux-musl` — a CROSS target, so the identity
    # compared two different builds and skipped. The next took the `host` note,
    # which is right today and is still a column position in a table formatted
    # for people.
    "$MCPP" toolchain list --format json 2>/dev/null \
        | jq -r '.data.host // empty'
}

ldflags_of() {   # extra args… → the ldflags line, or nothing
    rm -rf target
    "$MCPP" build "$@" >/dev/null 2>&1 || true
    local f; f="$(find target -name build.ninja 2>/dev/null | head -1)"
    # ⚠️ AN EXPLICIT `return 0`, BECAUSE THIS FUNCTION IS ALLOWED TO FIND
    # NOTHING. The first draft ended on `[ -n "$f" ] && grep …`, whose exit
    # status under `set -e` is the test's — so the one case this test exists to
    # examine, a build that produced no link line, killed the script before it
    # could say so. It exited 1 with no output at all.
    if [ -n "$f" ]; then
        grep -m1 '^ldflags' "$f" || true
    fi
    return 0
}

fail=0
checked=0

for tc in gcc llvm; do
    # ⚠️ THE TOOLCHAIN IS NAMED, because the defect lives on one of them and a
    # run that silently used the other would pass while proving nothing.
    # ⭐ ONE FIELD, NOT A COLUMN. Earlier drafts read `$NF` — which is
    # `(default)` on exactly the row most likely to be picked — and then
    # `grep -oP`, which does not exist on macOS and would have turned this half
    # into a silent skip there.
    ver="$("$MCPP" toolchain list --format json 2>/dev/null \
           | jq -r --arg t "$tc" '[.data.toolchains[] | select(.family==$t) | .version][0] // empty')"
    if [ -z "$ver" ]; then
        echo "  SKIP  $tc is not installed here"
        continue
    fi
    printf '[package]\nname    = "idprobe"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "%s@%s"\n' \
        "$tc" "$ver" > mcpp.toml

    ht="$(host_target)"
    if [ -z "$ht" ]; then
        echo "  SKIP  could not read this host's own target from mcpp"
        continue
    fi

    implicit="$(ldflags_of)"
    explicit="$(ldflags_of --target "$ht")"

    if [ -z "$implicit" ] || [ -z "$explicit" ]; then
        # ⚠️ EARNED, NOT ASSUMED. One of the two produced no build.ninja at all,
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
    # `--target=<triple>` itself is expected on the explicit side and only
    # there: it is the one token that names which machine, and the identity is
    # about everything else.
    a="$(printf '%s\n' "$implicit" | tr ' ' '\n' | grep -v '^--target=' | sort)"
    b="$(printf '%s\n' "$explicit" | tr ' ' '\n' | grep -v '^--target=' | sort)"

    if [ "$a" = "$b" ]; then
        echo "  ok  $tc@$ver: naming $ht changes nothing"
    else
        echo "FAIL: $tc@$ver: naming $ht changed the link line"
        diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") \
            | grep -E '^[<>]' | head -8 | sed 's/^/        /'
        fail=1
    fi
done

if [ "$checked" = 0 ]; then
    echo "SKIP: no toolchain here produced a link line to compare"
    exit 0
fi
[ "$fail" = 0 ] || exit 1
echo "OK: naming the host's own target changes nothing ($checked toolchains)"
