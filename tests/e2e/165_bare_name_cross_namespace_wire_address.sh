#!/usr/bin/env bash
# requires:
# 165_bare_name_cross_namespace_wire_address.sh — SPEC-001 §6: the descriptor
# the identity gate accepted supplies BOTH halves of the wire address.
#
# THE INCIDENT THIS LOCKS DOWN (2026-07-25, 6 of 7 workflows red on main):
#
# A bare dependency (`gtest = "1.15.2"`) resolves to the DEFAULT namespace
# (`mcpplibs`), but the identity gate deliberately also accepts a `compat`
# descriptor for it — that is how every `compat.*` package is consumable
# without writing the namespace out. mcpp then took the wire NAME from that
# descriptor and the wire NAMESPACE from the request, emitting
# `mcpplibs:gtest`, which no index is keyed by.
#
# It passed for months by coincidence: the literal `package.name` used to read
# `compat.gtest`, so the hardcoded `compat.<short>` retry caught it. When
# mcpp-index migrated to SPEC-001 short names (`name = "gtest"`), that
# coincidence vanished and every bare-name dependency served by a compat
# descriptor became uninstallable — 34 of the index's 48 packages.
#
# WHY THIS TEST IS HERMETIC. The index side validated that migration against
# its own examples, all of which spell the dependency `[dependencies.compat]`
# — the one form that was never broken. Verification matrix and consumption
# matrix were not the same shape, so 8 green checks meant nothing. This test
# builds its own index so it cannot drift with the live one, and it asserts on
# the address mcpp SENDS, not just on whether the install happened to work.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Reuse a real, downloadable descriptor so this exercises the whole install
# path rather than stopping at resolution.
SRC="$MCPP_HOME/registry/data/mcpplibs/pkgs/c/chriskohlhoff.asio.lua"
if [ ! -f "$SRC" ]; then
    echo "SKIP 165_bare_name_cross_namespace_wire_address (no asio descriptor in registry)"
    exit 0
fi

# A `compat`-namespaced package named by its SPEC-001 short name. Filed under
# the `compat.<short>.lua` filename a bare request probes as a fallback
# candidate — exactly the live index's layout for all 34 compat packages.
mkdir -p idx/pkgs/c
sed -e 's/^\( *namespace *= *\)"[^"]*"/\1"compat"/' \
    -e 's/^\( *name *= *\)"[^"]*"/\1"widget"/' \
    "$SRC" > idx/pkgs/c/compat.widget.lua
grep -qE '^ *namespace *= *"compat"' idx/pkgs/c/compat.widget.lua || {
    echo "FAIL: fixture namespace rewrite missed (upstream descriptor shape changed?)"; exit 1; }
grep -qE '^ *name *= *"widget"' idx/pkgs/c/compat.widget.lua || {
    echo "FAIL: fixture name rewrite missed (upstream descriptor shape changed?)"; exit 1; }

# The consumer writes the dependency BARE — no namespace. `[indices] mcpplibs`
# points the default namespace at the fixture index (findIndexForNs routes the
# default namespace through this entry), so the whole thing stays offline.
mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[indices]
mcpplibs = { path = "../idx" }

[dependencies]
widget = "1.38.1"
EOF
echo 'int main() { return 0; }' > app/src/main.cpp

set +e
(cd app && MCPP_VERBOSE=1 "$MCPP" build > out.txt 2>&1)
build_rc=$?
set -e

# ── The assertion that matters: which address went on the wire ──────
#
# Checked independently of whether the install then succeeded, so this keeps
# reporting the real defect even when the network or the sandbox xlings is the
# thing that failed.
#
# Read the `"targets":[...]` payload specifically rather than grepping the log
# as a whole — the human-facing "Downloading …" and error lines carry the
# resolved identity `mcpplibs.widget`, which is correct there and would make a
# looser assertion answer the wrong question.
# Un-escape first: the interface command is single-quoted on unix
# (--args '{"targets":[...]}') but double-quoted with backslash-escaped quotes
# on Windows (--args "{\"targets\":[...]}"). Normalising makes one pattern
# work on both instead of silently matching nothing on one of them.
sed 's/\\"/"/g' app/out.txt | grep -oE '"targets":\["[^"]*"\]' > targets.txt || true
test -s targets.txt || {
    echo "FAIL: no install target was emitted at all"; cat app/out.txt; exit 1; }
if grep -q '"mcpplibs:widget@1\.38\.1"' targets.txt; then
    echo "FAIL: wire address took its namespace from the REQUEST (mcpplibs:widget)"
    echo "      The descriptor declares namespace=\"compat\"; the address must be compat:widget."
    cat targets.txt
    exit 1
fi
grep -q '"compat:widget@1\.38\.1"' targets.txt || {
    echo "FAIL: expected the wire address compat:widget@1.38.1"
    cat targets.txt
    exit 1; }

# ── And that the package actually installs under that identity ──────
if [ $build_rc -ne 0 ]; then
    echo "FAIL: build failed even though the wire address was correct"
    cat app/out.txt
    exit 1
fi

# xlings names an xpkg directory {namespace}-x-{literal name}. Search for it by
# NAME rather than at a fixed path — the store root differs per platform, and
# the directory's name is the whole point of the assertion.
find app/.mcpp -type d -name "compat-x-widget" 2>/dev/null | grep -q . || {
    echo "FAIL: expected a store dir named compat-x-widget"
    find app/.mcpp -type d -name "*-x-*" 2>/dev/null | head -10
    exit 1; }

echo "PASS 165_bare_name_cross_namespace_wire_address"
