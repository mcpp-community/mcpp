#!/usr/bin/env bash
# requires:
# 687 -- the vendored-xlings version probe is an argument vector.
#
# It was the command string `<xlings> --version 2>/dev/null`. On Windows every
# command string reaches cmd.exe, which cannot open `/dev/null`: it printed "The
# system cannot find the path specified." in every command after the first, did
# not run xlings, and returned an empty version. acquire_xlings_binary reads an
# empty version as "unknown, keep it", so a Windows home never replaced a
# vendored xlings older than the pin. Measured on windows-2022 with 2026.9.14.3.
#
# Criteria, in a home whose vendored xlings exists before the command runs:
#   A. The command's stderr carries no path error. The denominator is that the
#      vendored binary exists, so the probe did run.
#   B. A vendored binary whose `--version` answers a version older than the pin
#      is replaced from MCPP_VENDORED_XLINGS, and the command says so. The stand-in
#      is the ninja payload, whose `--version` prints a dotted version older than
#      any dated xlings. Not `subos/default/bin/ninja`: that is an xlings shim, one
#      multicall binary that answers as xlings once it is named `xlings`, and for
#      the same reason the real xlings is kept under its own name.
#   C. Without MCPP_VENDORED_XLINGS, an mcpp running from its release layout
#      (`<prefix>/bin/mcpp` beside `<prefix>/registry/bin/xlings`) replaces an
#      older vendored binary with the released one (mcpp#660). Before, the only
#      sources were the override variable and the PATH, so a home outside the
#      release directory kept whatever older xlings the PATH offered.
# A and B discriminate on Windows; elsewhere they are the control legs. C
# discriminates everywhere.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac

export MCPP_HOME="$TMP/mcpp-home"
# Offline: configuration loading still acquires the vendored xlings (it copies
# a local binary), and no bootstrap step reaches the network.
export MCPP_OFFLINE=1
# The home's own configuration, whose xlings binary is the vendored one. An
# inherited configuration may name another (the macOS runner's names its
# `~/.xlings` shim), and then nothing is vendored and nothing is probed.
MCPP_INHERIT_CONFIG=0 source "$(dirname "$0")/_inherit_toolchain.sh"
cd "$TMP"

VENDORED="$MCPP_HOME/registry/bin/xlings$EXE"

"$MCPP" self env > first.out 2> first.err || true
[ -f "$VENDORED" ] || fail "the first command did not vendor xlings at $VENDORED" first.out first.err

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" self env > second.out 2> second.err || true
if grep -qi 'cannot find the path specified' second.err; then
    fail "A: the version probe reached a shell that could not open its redirect" second.err
fi
echo "ok: A, the probe ran and wrote nothing to stderr"

# ── B ──────────────────────────────────────────────────────────────────────
NINJA=""
for cand in "$MCPP_HOME"/registry/data/xpkgs/xim-x-ninja/*/ninja$EXE \
            "$MCPP_HOME"/registry/data/xpkgs/xim-x-ninja/*/bin/ninja$EXE; do
    if [ -f "$cand" ]; then NINJA="$cand"; break; fi
done
[ -n "$NINJA" ] || fail "B: no ninja binary to stand in for an older xlings under $MCPP_HOME/registry"
older=$("$NINJA" --version 2>/dev/null | head -1)
case "$older" in
    [0-9]*.*) ;;
    *) fail "B: the stand-in '$NINJA' answered '$older', not a dotted version" ;;
esac

mkdir -p "$TMP/real"
cp "$VENDORED" "$TMP/real/xlings$EXE"
rm -f "$VENDORED"
cp "$NINJA" "$VENDORED"
chmod +x "$VENDORED" 2>/dev/null || true

MCPP_VENDORED_XLINGS="$TMP/real/xlings$EXE" "$MCPP" self env > third.out 2> third.err || true
grep -q "vendored xlings $older -> " third.err \
    || fail "B: a vendored xlings answering $older was not replaced" third.out third.err
"$VENDORED" --version 2>/dev/null | grep -q '^xlings ' \
    || fail "B: after the replacement the vendored binary is not xlings" third.err
echo "ok: B, a vendored binary older than the pin was replaced"

# ── C ──────────────────────────────────────────────────────────────────────
rm -f "$VENDORED"
cp "$NINJA" "$VENDORED"
chmod +x "$VENDORED" 2>/dev/null || true
mkdir -p "$TMP/release/bin" "$TMP/release/registry/bin"
cp "$MCPP" "$TMP/release/bin/mcpp$EXE"
cp "$TMP/real/xlings$EXE" "$TMP/release/registry/bin/xlings$EXE"
chmod +x "$TMP/release/bin/mcpp$EXE" "$TMP/release/registry/bin/xlings$EXE" 2>/dev/null || true

# The PATH is reduced to the system directories, so an xlings on the PATH
# cannot be the source that makes C pass.
C_PATH="/usr/bin:/bin"
if PATH="$C_PATH" command -v xlings > /dev/null 2>&1; then
    fail "C: an xlings is reachable on $C_PATH, so the criterion cannot tell its sources apart"
fi
env -u MCPP_VENDORED_XLINGS PATH="$C_PATH" "$TMP/release/bin/mcpp$EXE" self env > fourth.out 2> fourth.err || true
grep -q "vendored xlings $older -> " fourth.err \
    || fail "C: the released xlings did not replace a vendored xlings answering $older" fourth.out fourth.err
"$VENDORED" --version 2>/dev/null | grep -q '^xlings ' \
    || fail "C: after the replacement the vendored binary is not xlings" fourth.err
echo "ok: C, the xlings released with this mcpp replaced an older vendored one"
