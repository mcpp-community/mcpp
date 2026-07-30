#!/usr/bin/env bash
# requires:
# 163_identity_first_resolution.sh — SPEC-001 §3.4 / §6: a package is found by
# its DECLARED identity `(namespace, name)`, and addressed on the wire by the
# LITERAL `package.name`.
#
# Three properties, none of which held before mcpp 0.0.106:
#
#   1. A short-name descriptor (`namespace="acme", name="widget"`) installs.
#      Previously mcpp re-derived the wire name as `<ns>.<short>` and asked the
#      index for `acme.widget`, which it is not keyed by → E_NOT_FOUND (#278).
#   2. The filename is only a hint. A descriptor called anything, in any letter
#      directory, is still discovered by its declared identity. Previously
#      discovery probed a fixed candidate-filename list, so such a file was
#      invisible even though the identity gate would have accepted it.
#   3. Two packages sharing a short name under different namespaces coexist in
#      ONE index and are each addressable. This needs xlings >= 0.4.69
#      (openxlings/xlings#381); mcpp's part is sending `<ns>:<literal name>`
#      rather than a reconstructed string.
#
# The legacy fully-qualified spelling must keep working throughout — every
# currently published descriptor uses it.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A real, downloadable payload so this exercises the whole install path rather
# than stopping at resolution. Reuse whatever asio the registry already has.
SRC=""
for c in "$MCPP_HOME/registry/data/mcpplibs/pkgs/c/chriskohlhoff.asio.lua"; do
    [ -f "$c" ] && SRC="$c" && break
done
if [ -z "$SRC" ]; then
    echo "SKIP 163_identity_first_resolution (no asio descriptor in registry)"
    exit 0
fi

mkidx() {  # mkidx <dir> <relpath> <namespace> <name>
    mkdir -p "$1/pkgs/$(dirname "$2")"
    # Rewrite the identity fields by SHAPE, never by their current values. The
    # first version of this matched the literal `name = "chriskohlhoff.asio"`,
    # which silently stopped matching the day mcpp-index migrated that
    # descriptor to its SPEC-001 short name. The fixture then carried the WRONG
    # identity and this test failed on three platforms for a reason that had
    # nothing to do with what it asserts.
    sed -e "s/^\( *namespace *= *\)\"[^\"]*\"/\1\"$3\"/" \
        -e "s/^\( *name *= *\)\"[^\"]*\"/\1\"$4\"/" \
        "$SRC" > "$1/pkgs/$2"
    # Fail loudly rather than go on to test a fixture that isn't what we asked
    # for — a missed rewrite must never masquerade as a resolution bug.
    grep -qE "^ *namespace *= *\"$3\"" "$1/pkgs/$2" || {
        echo "FAIL: fixture namespace rewrite missed (upstream descriptor shape changed?)"
        exit 1; }
    grep -qE "^ *name *= *\"$4\"" "$1/pkgs/$2" || {
        echo "FAIL: fixture name rewrite missed (upstream descriptor shape changed?)"
        exit 1; }
}

mkapp() {  # mkapp <dir> <indexname> <indexpath> <ns> <short>
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[indices]
$2 = { path = "$3" }

[dependencies.$4]
$5 = "1.38.1"
EOF
    echo 'int main() { return 0; }' > "$1/src/main.cpp"
}

# ── 1. short-name descriptor installs ───────────────────────────────
mkidx idx1 a/acme.widget.lua acme widget
mkapp app1 acme ../idx1 acme widget
(cd app1 && "$MCPP" build > out.txt 2>&1) || { cat app1/out.txt; exit 1; }
# Either verb: what this test is about is that the descriptor was found BY
# IDENTITY and the package became part of the build. Whether its objects were
# compiled here or served from the global build cache is a different subsystem's
# business — and now that the cache actually works, a sibling app dir under the
# same MCPP_HOME (or a restored CI sandbox) can legitimately supply them.
grep -qE "(Compiling|Cached) acme\.widget" app1/out.txt || {
    cat app1/out.txt
    echo "FAIL: acme.widget was neither compiled nor served from cache"
    exit 1
}
# xlings names the dir {namespace}-x-{literal name} — short name → acme-x-widget
test -d "app1/.mcpp/.xlings/data/xpkgs/acme-x-widget" \
    || { echo "FAIL: expected store dir acme-x-widget"; ls -R app1/.mcpp/.xlings/data/xpkgs 2>/dev/null; exit 1; }

# ── 2. legacy fully-qualified spelling still installs ───────────────
mkidx idx2 a/acme.widget.lua acme acme.widget
mkapp app2 acme ../idx2 acme widget
(cd app2 && "$MCPP" build > out.txt 2>&1) || { cat app2/out.txt; exit 1; }
# ...and lands under the dir its LITERAL name implies
test -d "app2/.mcpp/.xlings/data/xpkgs/acme-x-acme.widget" \
    || { echo "FAIL: expected store dir acme-x-acme.widget"; exit 1; }

# ── 3. filename is only a hint ──────────────────────────────────────
# Arbitrary basename, and deliberately the "wrong" letter directory.
mkidx idx3 z/totally-unrelated-name.lua acme widget
mkapp app3 acme ../idx3 acme widget
(cd app3 && "$MCPP" build > out.txt 2>&1) || {
    echo "FAIL: descriptor must be discoverable by identity, not filename"
    cat app3/out.txt; exit 1; }

# ── 4. two namespaces, one short name, ONE index ────────────────────
# Needs xlings >= 0.4.69 (openxlings/xlings#381): before it, one index repo
# keyed its table by the bare `package.name`, so the second package was
# unreachable even with an explicit namespace.
#
# The sandbox xlings comes from bootstrapping the PREVIOUS mcpp release, so
# until 0.0.106 ships (bundling 0.4.69) CI still runs 0.4.68 here. Skip this
# sub-case rather than assert a capability the sandbox cannot have yet —
# properties 1-3 above are the ones this change is responsible for.
XLBIN="${MCPP_HOME:-$HOME/.mcpp}/registry/bin/xlings"
XLVER=""
[ -x "$XLBIN" ] && XLVER=$("$XLBIN" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
xl_ge_0469() {
    [ -n "$XLVER" ] || return 1
    printf '%s\n0.4.69\n' "$XLVER" | sort -V | head -1 | grep -qx "0.4.69"
}
if ! xl_ge_0469; then
    echo "PASS 163_identity_first_resolution (step 4 skipped: sandbox xlings ${XLVER:-unknown} < 0.4.69)"
    exit 0
fi

mkidx idx4 a/alpha.widget.lua alpha widget
mkidx idx4 b/beta.widget.lua  beta  widget

mkapp app4a alpha ../idx4 alpha widget
(cd app4a && "$MCPP" build > out.txt 2>&1) || { cat app4a/out.txt; exit 1; }
test -d "app4a/.mcpp/.xlings/data/xpkgs/alpha-x-widget" \
    || { echo "FAIL: alpha:widget should land in alpha-x-widget"; exit 1; }

mkapp app4b beta ../idx4 beta widget
(cd app4b && "$MCPP" build > out.txt 2>&1) || {
    echo "FAIL: beta:widget unreachable — needs xlings >= 0.4.69 (#381)"
    cat app4b/out.txt; exit 1; }
test -d "app4b/.mcpp/.xlings/data/xpkgs/beta-x-widget" \
    || { echo "FAIL: beta:widget should land in beta-x-widget"; exit 1; }

echo "PASS 163_identity_first_resolution"
