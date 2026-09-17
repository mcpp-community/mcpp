#!/usr/bin/env bash
# requires: gcc
# 740 -- when the target's C library comes from the dependency graph and the
# resolved compiler cannot isolate it from the host's own (GCC: no single
# flag equivalent to Clang's `-nostdlibinc`), mcpp reports a degradation and
# proceeds. It neither refuses the build nor stays silent.
#
# THE HISTORY, BECAUSE BOTH OTHER SHAPES WERE TRIED AND MEASURED WRONG.
#
# Silent was the state before mcpp#662's M1: GCC never emitted the isolation
# token (hostflags.cppm's `bypassCfg` is unconditionally false for GCC, no
# `<driver>.cfg` beside it), and nothing said so.
#
# A hard refusal at `prepare_build` was tried next, reasoning "never
# silently unisolated" all the way to a build error. Measured against the
# existing suite it broke three files (268, 282, 303) that use a synthetic
# `provides = ["mcpp:c-abi=..."]` package with the AMBIENT default toolchain
# (gcc on a fresh Linux runner) to assert something about the target-side
# REPORT, none of them about isolation -- and the refusal fired before that
# report printed, which is exactly what 268's own comment says must never
# happen: "the resolution is reported during planning ... before a single
# object is compiled ... complete whether or not the link afterwards
# succeeds".
#
# `mcpp::diag::degraded` is the third option this test locks in: the build
# is not refused (268/282/303 stay green -- this file's own half two
# reproduces 303's shape and checks the report line still prints), and the
# gap is not silent either (half one).
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p libc/src
printf '[package]\nname     = "fake-libc"\nversion  = "0.1.0"\nprovides = ["mcpp:c-abi=musl"]\n\n[build]\nsources  = []\n' \
    > libc/mcpp.toml
printf '[package]\nname    = "clibdeg"\nversion = "0.1.0"\n\n[dependencies]\nfake-libc = { path = "libc" }\n' \
    > mcpp.toml

# `why toolchain`, NOT `build`: this file's subject is what mcpp REPORTS
# during resolution, and a real link additionally needs a working sandbox
# C-runtime setup that has nothing to do with the claim here (`build`'s own
# behaviour on this exact fixture is 303's territory).
out="$("$MCPP" why toolchain 2>&1)"

# ── Half one: the degradation names the library and the compiler ───────────
echo "$out" | grep -q '^warning: ' || {
    echo "FAIL: no warning at all on a GCC toolchain over a graph-supplied C library:"
    echo "$out"; exit 1
}
echo "$out" | grep -qF "C library ('musl'" || {
    echo "FAIL: the warning does not name the C library:"; echo "$out"; exit 1
}
echo "$out" | grep -qF "resolved compiler ('gcc')" || {
    echo "FAIL: the warning does not name the compiler that cannot isolate:"
    echo "$out"; exit 1
}
echo "$out" | grep -qi 'clang' || {
    echo "FAIL: the warning does not point at the toolchain that does isolate:"
    echo "$out"; exit 1
}
echo "  ok  a GCC toolchain over a graph-supplied C library is named, not silent"

# ── Half two: it is a note, not a refusal -- the report still reaches print ─
#
# THE EXACT FAILURE MODE THE HARD REFUSAL HAD: firing before `format_report`
# printed anything at all. Asserting the report line is present is what
# distinguishes "advisory" from "the build's replacement".
echo "$out" | grep -qE 'c-abi *musl' || {
    echo "FAIL: the target-side report did not print at all -- looks like the"
    echo "      refusal this file exists to keep reverted:"
    echo "$out"; exit 1
}
echo "  ok  the target-side report still prints (this is advisory, not a refusal)"

# ── Half three: the control -- a Clang-family toolchain gets no such note ──
#
# THE HARNESS'S OWN VALIDITY. Without this, half one could be passing because
# the message ALWAYS prints, on every toolchain, which would say nothing
# about GCC specifically.
llvmspec="$("$MCPP" toolchain list --format json 2>/dev/null \
    | jq -r '[.data.toolchains[] | select(.family=="llvm") | "llvm@"+.version]
              | unique | .[0] // empty' | tr -d '\r')"
if [ -n "$llvmspec" ]; then
    printf '[package]\nname    = "clibdeg"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "%s"\n\n[dependencies]\nfake-libc = { path = "libc" }\n' \
        "$llvmspec" > mcpp.toml
    out2="$("$MCPP" why toolchain 2>&1)"
    if echo "$out2" | grep -q "has no way to stop"; then
        echo "FAIL: the same degradation fired on a Clang-family toolchain, which"
        echo "      DOES isolate the graph's C library:"
        echo "$out2"; exit 1
    fi
    echo "  ok  a Clang-family toolchain over the same graph gets no such note"
else
    echo "SKIP: no llvm installed here to run the control against"
fi

echo "OK: 740 a compiler that cannot isolate a graph-supplied C library is degraded, not refused"
