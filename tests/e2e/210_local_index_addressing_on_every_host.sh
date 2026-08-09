#!/usr/bin/env bash
# requires:
# 210_local_index_addressing_on_every_host.sh — a `[indices]` path must address
# the same directory on every host, whether it is written absolute or inherited
# from a workspace root.
#
# This exists because the tests that covered local-index addressing all needed
# a compiler or a fresh sandbox, and Windows has neither capability — so the
# platform where path SEMANTICS actually differ was the one platform never
# asserting them. A fixture wrote an MSYS path into mcpp.toml, a native
# mcpp.exe read the leading `/` as "root of the current drive", and the
# resulting "package not found in any configured index" was chased for a day
# through the workspace inheritance code, which was not involved at all.
#
# So: no compiler, no sandbox bootstrap, no network. Everything here is
# assertable at the resolution layer, which is exactly the layer that was
# broken. `mcpp add` is the probe because its existence gate reads through the
# same routing `mcpp build` resolves dependencies with.
set -euo pipefail
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

make_index() {  # make_index <dir>
    mkdir -p "$1/pkgs/a"
    cat > "$1/pkgs/a/acme.util.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "acme",
    name = "util",
    description = "local index fixture",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["2.0.0"] = { url = "https://example.invalid/u.tar.gz" } },
        macosx  = { ["2.0.0"] = { url = "https://example.invalid/u.tar.gz" } },
        windows = { ["2.0.0"] = { url = "https://example.invalid/u.zip" } },
    },
}
EOF
}

# ── 1. an ABSOLUTE [indices] path ───────────────────────────────────
make_index "$TMP/abs-index"
INDEX_HOST="$(host_path "$TMP/abs-index")"
mkdir -p "$TMP/abs-app"
cat > "$TMP/abs-app/mcpp.toml" <<EOF
[package]
name = "absapp"
version = "0.1.0"

[indices]
acme = { path = "$INDEX_HOST" }
EOF
cd "$TMP/abs-app"

"$MCPP" add acme.util@2.0.0 > /dev/null || {
    echo "FAIL: an absolute local index path did not resolve on this host"
    "$MCPP" add acme.util@2.0.0 2>&1 | sed 's/^/  /'
    exit 1
}
grep -qE '^util = "2\.0\.0"$' mcpp.toml || {
    cat mcpp.toml; echo "FAIL: dependency was not written"; exit 1; }

# A miss must report the index as READABLE. "root absent" here would mean the
# path was addressed but does not exist — the exact fingerprint of a path
# written in the wrong spelling.
err=$("$MCPP" add acme.nope@1.0.0 2>&1) && {
    echo "FAIL: expected a miss for acme.nope"; exit 1; }
[[ "$err" == *"route: local index 'acme': root present, pkgs present"* ]] || {
    echo "$err"
    echo "FAIL: the absolute index is not being addressed as a readable root"
    exit 1
}

# ── 2. a workspace member inheriting a ROOT-RELATIVE index ──────────
# The member sees no [indices] of its own; the root's relative `path = "index"`
# is anchored at the ROOT, not at the member's directory.
mkdir -p "$TMP/ws"
make_index "$TMP/ws/index"
cat > "$TMP/ws/mcpp.toml" <<'EOF'
[workspace]
members = ["m1"]

[indices]
acme = { path = "index" }
EOF
mkdir -p "$TMP/ws/m1"
cat > "$TMP/ws/m1/mcpp.toml" <<'EOF'
[package]
name = "m1"
version = "0.1.0"
EOF
cd "$TMP/ws/m1"

"$MCPP" add acme.util@2.0.0 > /dev/null || {
    echo "FAIL: a workspace member could not read its root-owned local index"
    "$MCPP" add acme.util@2.0.0 2>&1 | sed 's/^/  /'
    exit 1
}
grep -qE '^util = "2\.0\.0"$' mcpp.toml || {
    cat mcpp.toml; echo "FAIL: member did not inherit the workspace [indices]"; exit 1; }

err=$("$MCPP" add acme.nope@1.0.0 2>&1) && {
    echo "FAIL: expected a miss inside the workspace member"; exit 1; }
[[ "$err" == *"route: local index 'acme': root present, pkgs present"* ]] || {
    echo "$err"
    echo "FAIL: the inherited index was anchored somewhere unreadable"
    exit 1
}

# ── 3. the route diagnostic stays privacy-safe ──────────────────────
# It is what makes a wrong path diagnosable at all, so it must be printed — and
# it must not print the path, which is why it says present/absent instead.
case "$err" in
    *"$TMP"* | *"$(host_path "$TMP")"*)
        echo "FAIL: the route diagnostic leaked a filesystem path"
        exit 1
        ;;
esac

echo "OK"
