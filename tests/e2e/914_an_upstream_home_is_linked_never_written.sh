#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 914 -- a home with an upstream installs from it by link, and never writes it
# (docs/25 §3).
#
# The upstream is an mcpp home of this user's (prepared from the machine's
# toolchain, as every isolated test home is). Criteria:
#   A. The isolated home builds and runs a program; its C runtime payload is a
#      link into the upstream's store, and the program's loader is the path in
#      the isolated home's own store.
#   B. The upstream's store is the same before and after (names, link targets).
#   C. `mcpp self doctor` names the upstream; `mcpp self store dedupe` finds
#      nothing to link that is not linked already.
#   D. An upstream another user could change is refused when the configuration
#      is read (a directory this user can write but does not own: /tmp).
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

# The upstream: an mcpp home holding the machine's toolchain.
UP="$TMP/upstream"
export MCPP_HOME="$UP"
source "$(dirname "$0")/_inherit_toolchain.sh"
"$MCPP" self env > "$TMP/up.env" 2>&1 || fail "the upstream home does not initialise" "$TMP/up.env"
snapshot() { (cd "$UP/registry/data/xpkgs" && find . -maxdepth 2 -printf '%p %l\n' | sort); }
snapshot > "$TMP/up.before"

# The home under test: nothing installed, the upstream named.
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
"$MCPP" self env > /dev/null 2>&1 || true
sed -i.bak "s|^\[xlings\]\$|[xlings]\nupstream = [\"$(host_path "$UP")\"]|" "$MCPP_HOME/config.toml"
grep -q '^upstream = ' "$MCPP_HOME/config.toml" || fail "the test could not name the upstream" "$MCPP_HOME/config.toml"

# ── A ─────────────────────────────────────────────────────────────────────────
cd "$TMP"
"$MCPP" new hello > new.log 2>&1 || fail "A: mcpp new" new.log
cd hello
"$MCPP" build > build.log 2>&1 || fail "A: the build failed" build.log
BIN=$(find target -path '*/bin/hello' -type f | head -1)
"$BIN" > run.log 2>&1 || fail "A: the program does not run" run.log
GLIBC=$(find "$MCPP_HOME/registry/data/xpkgs/xim-x-glibc" -mindepth 1 -maxdepth 1 | head -1)
[ -L "$GLIBC" ] || fail "A: the C runtime was installed here, not linked from the upstream ($GLIBC)"
case "$(readlink "$GLIBC")" in "$UP"/*|"$(host_path "$UP")"/*) ;; *) fail "A: $GLIBC links elsewhere: $(readlink "$GLIBC")" ;; esac
if command -v readelf > /dev/null; then
    readelf -lW "$BIN" | grep -q "interpreter: $MCPP_HOME/registry/data/xpkgs/xim-x-glibc/" \
        || fail "A: the program's loader is not the path in this home's store" <(readelf -lW "$BIN")
fi
echo "ok: A. built against the upstream's runtime, by link"

# ── B ─────────────────────────────────────────────────────────────────────────
snapshot > "$TMP/up.after"
cmp -s "$TMP/up.before" "$TMP/up.after" || { diff "$TMP/up.before" "$TMP/up.after"; fail "B: the upstream changed"; }
echo "ok: B. the upstream is unchanged"

# ── C ─────────────────────────────────────────────────────────────────────────
"$MCPP" self doctor > doctor.log 2>&1 || true
grep -q "upstream $(host_path "$UP")" doctor.log || fail "C: doctor does not name the upstream" doctor.log
"$MCPP" self store dedupe > dedupe.log 2>&1 || fail "C: dedupe failed" dedupe.log
grep -q "$MCPP_HOME/registry/data/xpkgs/xim-x-glibc" dedupe.log && fail "C: a linked payload is listed again" dedupe.log
echo "ok: C. doctor names it; dedupe finds the linked payloads linked"

# ── D ─────────────────────────────────────────────────────────────────────────
if [ "$(id -u)" != 0 ] && [ "$(stat -c %u /tmp 2>/dev/null || stat -f %u /tmp)" = 0 ]; then
    cp "$MCPP_HOME/config.toml" "$TMP/config.keep"
    sed -i.bak '/^upstream = /d' "$MCPP_HOME/config.toml"
    sed -i.bak 's|^\[xlings\]$|[xlings]\nupstream = ["/tmp"]|' "$MCPP_HOME/config.toml"
    if "$MCPP" self env > untrusted.log 2>&1; then fail "D: an upstream this user can write but does not own was used" untrusted.log; fi
    grep -q "upstream /tmp: not used" untrusted.log || fail "D: the refusal does not name the upstream" untrusted.log
    cp "$TMP/config.keep" "$MCPP_HOME/config.toml"
    echo "ok: D. an upstream another user could change is refused"
fi

echo "PASS: 914_an_upstream_home_is_linked_never_written"
