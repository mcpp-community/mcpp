#!/usr/bin/env bash
# requires: unix-shell
# 812_an_index_floor_is_a_closing_tip.sh — an index that requires a newer mcpp
# is not an error of the run.
#
# An index is data and mcpp is the program; `min_mcpp` routes, it does not
# terminate. Criteria:
#   A. A refresh that brings in a tree requiring a newer mcpp succeeds; the
#      guard keeps the previous tree, and the run ends with exactly one `tip:`
#      line, which is its last line of output. No `error:` line is printed.
#   B. `mcpp self doctor` lists an index whose floor this mcpp does not meet,
#      with the floor, and points at E0006.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"

DATA="$MCPP_HOME/indices/mcpplibs"
rm -rf "$DATA"
mkdir -p "$DATA/pkgs/f"
printf '[index]\nspec = "1"\nmin_mcpp = "0.0.1"\n' > "$DATA/index.toml"
printf 'package = { spec = "1", name = "fixture", type = "package" }\n' > "$DATA/pkgs/f/fixture.lua"

# The stub stands in for xlings: an owned index is refreshed into a staging
# directory (sync_index with a destination, docs/25 §2), and the copy it
# fetches requires a newer mcpp than any release, which is what a published
# floor bump looks like to an older client. Every other call succeeds and does
# nothing.
mkdir -p "$TMP/bin"
cat > "$TMP/bin/xlings" <<EOF
#!/usr/bin/env bash
# The handshake mcpp asks of every xlings (docs/25 §5).
[[ "\${1:-}" == "interface" && "\${2:-}" == "--version" ]] && { echo '{"protocol_version":"1.10","version":"9999.1.1.1","capabilities":["install_packages"],"home_layouts":[2],"features":["index_dir","index_store","upstreams","payload_status","sync_index"]}'; exit 0; }
case " \$* " in
    *" sync_index "*)
        dest=\$(printf '%s' "\$4" | sed -n 's/.*"dest":"\([^"]*\)".*/\1/p')
        mkdir -p "\$dest/pkgs/f"
        cp "$DATA/pkgs/f/fixture.lua" "\$dest/pkgs/f/fixture.lua"
        printf '[index]\nspec = "1"\nmin_mcpp = "9999.9.9.9"\n' > "\$dest/index.toml"
        echo '{"kind":"result","exitCode":0}'
        ;;
    *" interface "*) echo '{"kind":"result","exitCode":0}' ;;
esac
exit 0
EOF
chmod +x "$TMP/bin/xlings"
STUB_HOST="$(host_path "$TMP/bin/xlings")"
{
    grep -v '^binary' "$MCPP_HOME/config.toml" 2>/dev/null | sed '/^\[xlings\]/d'
    printf '\n[xlings]\nbinary = "%s"\n' "$STUB_HOST"
} > "$TMP/config.toml"
mv "$TMP/config.toml" "$MCPP_HOME/config.toml"

# ── A. the refresh ends with one tip, and no error ──────────────────────────
cd "$TMP"
"$MCPP" index update > a.out 2> a.err || fail "A: the refresh failed" a.err
grep -q '^error:' a.err && fail "A: a refresh that kept a usable index printed an error" a.err
[ "$(grep -c '^tip:' a.err)" = 1 ] || fail "A: expected exactly one tip line" a.err
tail -1 a.err | grep -q '^tip: .*requires a newer mcpp' \
    || fail "A: the tip is not the last line of the run" a.err
grep -q '^tip: .*requires mcpp >= 9999.9.9.9' a.err \
    || fail "A: the tip does not name the version the index asks for" a.err
grep -q 'min_mcpp = "0.0.1"' "$DATA/index.toml" \
    || fail "A: the guard did not keep the previous, usable tree" "$DATA/index.toml"
echo "ok: A. a floor bump seen by a refresh is one closing tip"

# ── B. doctor reports the state ─────────────────────────────────────────────
printf '[index]\nspec = "1"\nmin_mcpp = "9999.9.9.9"\n' > "$DATA/index.toml"
"$MCPP" self doctor > b.out 2>&1 || true
grep -q "index 'mcpplibs' requires mcpp >= 9999.9.9.9" b.out \
    || fail "B: doctor does not list the index this mcpp cannot read" b.out
grep -q 'E0006' b.out || fail "B: doctor does not point at E0006" b.out
echo "ok: B. doctor lists an index whose floor this mcpp does not meet"

echo "PASS: 812_an_index_floor_is_a_closing_tip"
