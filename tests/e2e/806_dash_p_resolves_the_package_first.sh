#!/usr/bin/env bash
# requires: gcc
# 806 -- `-p, --package <NAME>` resolves a member's package identity first, and
# its directory (docs/07 §5.3's historical spellings) only as a fallback
# (mcpp#725). Before the fix, both resolvers (src/build/prepare/manifest.cpp,
# src/project.cppm resolve_member_dir) matched only a member's directory
# basename or its `[workspace] members` path -- the option's own name,
# `--package`, promised a package that neither ever read.
set -e
# The project-root copy of the compile database is opt-in since
# 2026.10.26.1 ([build] compile_commands); this test reads it.
export MCPP_COMPILE_COMMANDS=compile_commands.json

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws" && cd "$TMP/ws"

member() {   # member <dir> <package-name> [<namespace>]
    local dir="$1" name="$2" ns="${3:-}"
    mkdir -p "$dir"
    {
        echo "[package]"
        [ -n "$ns" ] && echo "namespace = \"$ns\""
        echo "name = \"$name\""
        echo "version = \"0.1.0\""
        echo
        echo "[targets.$name]"
        echo "kind = \"lib\""
        echo
        echo "[build]"
        echo "sources = [\"x.cpp\"]"
    } > "$dir/mcpp.toml"
    echo "int ${name//[^a-zA-Z0-9_]/_}_x() { return 0; }" > "$dir/x.cpp"
}

# `modules/base`'s package is `ws-base`; `ns1/common` and `ns2/common` share
# the bare package name `ws-common` under two different namespaces.
member "modules/base" "ws-base"
member "ns1/common" "ws-common" "ns1"
member "ns2/common" "ws-common" "ns2"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["modules/base", "ns1/common", "ns2/common"]
EOF

# Which member a `-p` selected is read from the workspace's compile database,
# which a build from a clean workspace fills with that member's closure alone
# (workspace design 2026-09-29 §7.1).
fresh() { rm -rf target compile_commands.json; }
built() {  # built <member dir>: the member's source is in the database
    [ -f compile_commands.json ] && grep -q "$1/x.cpp" compile_commands.json
}

# ── package name, directory basename, and full path all select the member ──
for filter in ws-base base modules/base; do
    fresh
    "$MCPP" build -p "$filter" > "sel-$(basename "$filter").log" 2>&1 \
        || fail "-p $filter did not build" "sel-$(basename "$filter").log"
    built modules/base \
        || fail "-p $filter did not select modules/base"
    grep -qi 'warning' "sel-$(basename "$filter").log" \
        && fail "-p $filter warned when no other member could conflict" \
                 "sel-$(basename "$filter").log"
done
echo "ok: package name, basename and path all select the one member"

# ── two members share a bare name under different namespaces ───────────────
"$MCPP" build -p ws-common > ambiguous.log 2>&1 \
    && fail "-p ws-common should have been refused as ambiguous" ambiguous.log
grep -q 'ns1.ws-common' ambiguous.log \
    || fail "the ambiguity refusal must name 'ns1.ws-common'" ambiguous.log
grep -q 'ns2.ws-common' ambiguous.log \
    || fail "the ambiguity refusal must name 'ns2.ws-common'" ambiguous.log

fresh
"$MCPP" build -p ns1.ws-common > qns1.log 2>&1 || fail "-p ns1.ws-common did not build" qns1.log
built ns1/common || fail "-p ns1.ws-common did not select ns1/common"
fresh
"$MCPP" build -p ns2.ws-common > qns2.log 2>&1 || fail "-p ns2.ws-common did not build" qns2.log
built ns2/common || fail "-p ns2.ws-common did not select ns2/common"
echo "ok: the bare name is refused; each qualified name selects its own member"

# ── a second member's directory basename collides with the first's package ─
member "dup/ws-base" "other"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["modules/base", "ns1/common", "ns2/common", "dup/ws-base"]
EOF
fresh

"$MCPP" build -p ws-base > name-vs-dir.log 2>&1 \
    || fail "-p ws-base did not build once a same-named directory existed" name-vs-dir.log
built modules/base \
    || fail "-p ws-base must still select the package 'ws-base' (modules/base)"
built dup/ws-base \
    && fail "-p ws-base must not have built dup/ws-base"
grep -qi 'warning' name-vs-dir.log \
    || fail "-p ws-base must warn once a directory shares its spelling" name-vs-dir.log
grep -q 'dup/ws-base' name-vs-dir.log \
    || fail "the warning must name the other member's path ('dup/ws-base')" name-vs-dir.log
echo "ok: the package name outranks another member's directory, with a warning"

echo "PASS: 806_dash_p_resolves_the_package_first"
