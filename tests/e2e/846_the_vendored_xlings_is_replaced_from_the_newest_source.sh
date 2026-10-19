#!/usr/bin/env bash
# requires:
# 846 -- a vendored xlings older than the pin is replaced from the newest source
# (mcpp#744).
#
# The check that decides whether to replace the vendored binary took the first
# source that existed -- MCPP_VENDORED_XLINGS, then the xlings released beside
# the running mcpp, then the PATH -- not the newest. A released copy older than
# the pin therefore hid a newer xlings on the PATH, and mcpp stated that no
# newer source was available. One function now chooses: the override when set,
# otherwise the newer of the released copy and the PATH copy.
#
# The stand-in for an older xlings is the ninja payload, whose `--version`
# prints a dotted version older than any dated xlings (as in e2e 687); the
# newer one is the real xlings. Both are real executables, so the criteria
# hold on Windows as well.
#
# Criteria, each from an mcpp running from its release layout
# (`<prefix>/bin/mcpp` beside `<prefix>/registry/bin/xlings`):
#   D. Released older, PATH newer: one `Updating` line naming the PATH, and the
#      vendored binary is then an xlings.
#   E. Released newer, PATH older: the released copy is taken.
#   F. Everything older: one `Note` line, and the vendored binary is kept.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac

export MCPP_HOME="$TMP/mcpp-home"
export MCPP_OFFLINE=1
MCPP_INHERIT_CONFIG=0 source "$(dirname "$0")/_inherit_toolchain.sh"
cd "$TMP"

VENDORED="$MCPP_HOME/registry/bin/xlings$EXE"
"$MCPP" self init > setup.out 2> setup.err || true
[ -f "$VENDORED" ] || fail "setup: the first command did not vendor xlings at $VENDORED" setup.out setup.err
"$VENDORED" --version 2>/dev/null | grep -q '^xlings ' \
    || fail "setup: the vendored binary does not answer as xlings" setup.err

NINJA=""
for cand in "$MCPP_HOME"/registry/data/xpkgs/xim-x-ninja/*/ninja$EXE \
            "$MCPP_HOME"/registry/data/xpkgs/xim-x-ninja/*/bin/ninja$EXE; do
    if [ -f "$cand" ]; then NINJA="$cand"; break; fi
done
[ -n "$NINJA" ] || fail "setup: no ninja binary to stand in for an older xlings"
older=$("$NINJA" --version 2>/dev/null | head -1)
case "$older" in
    [0-9]*.*) ;;
    *) fail "setup: the stand-in '$NINJA' answered '$older', not a dotted version" ;;
esac

mkdir -p "$TMP/real" "$TMP/release/bin" "$TMP/release/registry/bin" "$TMP/pathbin"
cp "$VENDORED" "$TMP/real/xlings$EXE"
cp "$MCPP" "$TMP/release/bin/mcpp$EXE"
chmod +x "$TMP/release/bin/mcpp$EXE" 2>/dev/null || true

BASE_PATH="/usr/bin:/bin"
# On Windows mcpp finds the PATH copy with `where`, which lies in System32, a
# directory every Windows PATH holds.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) BASE_PATH="$BASE_PATH:/c/Windows/System32" ;; esac
if PATH="$BASE_PATH" command -v xlings > /dev/null 2>&1; then
    fail "an xlings is reachable on $BASE_PATH, so the criteria cannot tell the sources apart"
fi

# place <file> <source-binary>: copy an executable into place.
place() { rm -f "$1"; cp "$2" "$1"; chmod +x "$1" 2>/dev/null || true; }
run() {   # run <name>: the release-layout mcpp, with the test PATH
    env -u MCPP_VENDORED_XLINGS PATH="$TMP/pathbin:$BASE_PATH" \
        "$TMP/release/bin/mcpp$EXE" self init > "$1.out" 2> "$1.err" || true
}
count() { grep -c "$1" "$2" || true; }

# ── D ──────────────────────────────────────────────────────────────────────
place "$VENDORED" "$NINJA"
place "$TMP/release/registry/bin/xlings$EXE" "$NINJA"
place "$TMP/pathbin/xlings$EXE" "$TMP/real/xlings$EXE"
run d
[ "$(count "vendored xlings $older -> " d.err)" = 1 ] \
    || fail "D: expected one Updating line for a vendored xlings answering $older" d.err
grep -q "vendored xlings $older -> .* from PATH" d.err \
    || fail "D: the replacement did not come from the newer xlings on the PATH" d.err
[ "$(count 'no newer source is available' d.err)" = 0 ] \
    || fail "D: mcpp stated that no newer source was available while one was on the PATH" d.err
"$VENDORED" --version 2>/dev/null | grep -q '^xlings ' \
    || fail "D: after the replacement the vendored binary is not xlings" d.err
echo "ok: D, a newer xlings on the PATH replaced the vendored one past an older released copy"

# ── E ──────────────────────────────────────────────────────────────────────
place "$VENDORED" "$NINJA"
place "$TMP/release/registry/bin/xlings$EXE" "$TMP/real/xlings$EXE"
place "$TMP/pathbin/xlings$EXE" "$NINJA"
run e
grep -q "vendored xlings $older -> .* from the release of this mcpp" e.err \
    || fail "E: the newer released copy was not taken over an older PATH copy" e.err
"$VENDORED" --version 2>/dev/null | grep -q '^xlings ' \
    || fail "E: after the replacement the vendored binary is not xlings" e.err
echo "ok: E, the newer released copy was taken"

# ── F ──────────────────────────────────────────────────────────────────────
place "$VENDORED" "$NINJA"
place "$TMP/release/registry/bin/xlings$EXE" "$NINJA"
place "$TMP/pathbin/xlings$EXE" "$NINJA"
run f
[ "$(count 'no newer source is available' f.err)" = 1 ] \
    || fail "F: expected exactly one Note when every source is older" f.err
[ "$(count 'vendored xlings .* -> ' f.err)" = 0 ] \
    || fail "F: a vendored binary was replaced by a source that is not newer" f.err
"$VENDORED" --version 2>/dev/null | grep -q '^xlings ' \
    && fail "F: the vendored binary changed although no source was newer" f.err
echo "ok: F, one Note and the vendored binary kept when no source is newer"
