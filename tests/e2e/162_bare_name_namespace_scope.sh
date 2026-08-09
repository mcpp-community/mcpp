#!/usr/bin/env bash
# requires:
# 162_bare_name_namespace_scope.sh — INV-RESOLVE (#278): a bare dependency name
# resolves in exactly one place (mcpplibs); every other namespace must be
# written out.
#
# Before this, a bare name that matched nothing fell through to the FIRST
# candidate SILENTLY, so mcpp carried on with a namespace it had invented and
# the user met the failure much later, wrapped around that invented name. And
# the discovery rung `(no namespace, X)` accepted a descriptor from ANY
# namespace, which made resolution depend on which indices happened to be
# present — adding an index could silently retarget an existing dependency.
#
# Covered: the hard failure, the did-you-mean hint (a DIAGNOSTIC-only index
# scan that must never feed resolution), and that both documented spellings
# resolve the very same package the hint names.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A path index owning namespace `acme`. Note the descriptor is FQN-correct —
# this test is about the CONSUMER's spelling, not the descriptor's.
mkdir -p idx/pkgs/a
cat > idx/pkgs/a/acme.widget.lua <<'EOF'
package = {
    spec = "1", namespace = "acme", name = "acme.widget",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            macosx = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            windows = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { schema = "0.1", sources = { "*.cpp" } },
}
EOF

mkdir -p app/src && cd app
echo 'int main() { return 0; }' > src/main.cpp

write_manifest() {
    cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[indices]
acme = { path = "../idx" }

$1
EOF
}

# ── 1. bare name for a third-party namespace: hard failure ──────────
write_manifest '[dependencies]
widget = "1.0.0"'

if "$MCPP" build > bare.out 2>&1; then
    echo "FAIL: a bare name must not reach the acme namespace"; cat bare.out; exit 1
fi
grep -q "no package found for exact selector" bare.out || {
    echo "FAIL: expected the explicit not-found error"; cat bare.out; exit 1; }
# The identities actually attempted must be listed — the old silent fallback
# reported a namespace the user never wrote.
grep -q "tried:" bare.out || { cat bare.out; exit 1; }

# ── 2. did-you-mean names the real package and both spellings ───────
grep -q "acme.widget" bare.out || {
    echo "FAIL: expected did-you-mean to name acme.widget"; cat bare.out; exit 1; }
grep -q "\[dependencies.acme\]" bare.out || {
    echo "FAIL: expected the sub-table spelling in the hint"; cat bare.out; exit 1; }

# ── 3. the dotted spelling the hint suggests resolves ───────────────
write_manifest '[dependencies]
"acme.widget" = "1.0.0"'
"$MCPP" build > dotted.out 2>&1 || true
# The asset URL is a sentinel, so the fetch fails — but resolution must have
# gotten far enough to ADDRESS the package, which is what this asserts.
if grep -q "no package found for exact selector" dotted.out; then
    echo "FAIL: dotted selector must resolve to (acme, widget)"; cat dotted.out; exit 1
fi

# ── 4. the sub-table spelling resolves identically ──────────────────
write_manifest '[dependencies.acme]
widget = "1.0.0"'
"$MCPP" build > subtable.out 2>&1 || true
if grep -q "no package found for exact selector" subtable.out; then
    echo "FAIL: sub-table form must resolve to (acme, widget)"; cat subtable.out; exit 1
fi

# ── 5. namespace omission is exactly mcpplibs ───────────────────────
# A bare gtest request must not cross into compat. The explicit dotted selector
# then proves the package remains reachable without ambiguity.
#
# Asserted on RESOLUTION, not on a successful build: whether the asset actually
# downloads depends on network/cache state, but "did the bare name reach the
# compat descriptor" does not. Keeping the assertion at the resolution layer is
# both the property under test and what makes this test hermetic.
write_manifest '[dependencies]
gtest = "1.15.2"'
"$MCPP" build > compat.out 2>&1 || true
grep -q "no package found for exact selector" compat.out || {
    echo "FAIL: bare gtest must remain inside mcpplibs"; cat compat.out; exit 1; }

write_manifest '[dependencies.compat]
gtest = "1.15.2"'
"$MCPP" build > compat-explicit.out 2>&1 || true
if grep -q "no package found for exact selector" compat-explicit.out; then
    echo "FAIL: explicit compat.gtest must resolve"; cat compat-explicit.out; exit 1
fi

echo "PASS 162_bare_name_namespace_scope"
