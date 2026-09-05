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

# A path index owning namespaces `acme` and `compat`. Note the acme descriptor
# is FQN-correct — this test is about the CONSUMER's spelling, not the
# descriptor's.
mkdir -p idx/pkgs/a idx/pkgs/c
cat > idx/pkgs/a/acme.widget.lua <<'EOF'
package = {
    spec = "1", namespace = "acme", name = "acme.widget",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            macosx = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            windows = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { schema = "0.1", sources = { "*.cpp" } },
}
EOF
# The compat wrapper shape every published `compat.*` package uses. Served from
# the fixture rather than the shared registry so section 5 asserts the same
# property offline that it used to assert against whatever gtest happened to be
# installed.
cat > idx/pkgs/c/compat.gadget.lua <<'EOF'
package = {
    spec = "1", namespace = "compat", name = "gadget",
    xpm = { linux = { ["2.0.0"] = { url = "u", sha256 = "h" } },
            macosx = { ["2.0.0"] = { url = "u", sha256 = "h" } },
            windows = { ["2.0.0"] = { url = "u", sha256 = "h" } } },
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
compat = { path = "../idx" }

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
# #487: the hint must also carry what the suggested package publishes —
# the fixture's xpm table lists exactly one version per OS.
grep -q "acme.widget (1.0.0)" bare.out || {
    echo "FAIL: expected did-you-mean to show acme.widget's version"; cat bare.out; exit 1; }
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

# ── 5. namespace omission means mcpplibs, with a one-release exit ramp ──
#
# Every published `compat.*` package and every manifest written before exact
# identity spells its dependency bare. Turning that into an immediate hard
# error would make an mcpp upgrade break builds against data that is already
# published and can no longer be edited — the mirror image of the rule that
# keeps a raised index floor from bricking older clients.
#
# So a bare name still REACHES compat for one release, but it can no longer do
# so quietly: that silence, not the reach, was the #278 defect. The hit is
# announced, it names the exact replacement, and the canonical identity is what
# travels downstream.
#
# Asserted on RESOLUTION, not on a successful build: whether the sentinel asset
# downloads is irrelevant to whether the bare name addressed the package.
write_manifest '[dependencies]
gadget = "2.0.0"'
"$MCPP" build > compat.out 2>&1 || true
if grep -q "no package found for exact selector" compat.out; then
    echo "FAIL: bare gadget must still reach compat during the migration window"
    cat compat.out; exit 1
fi
grep -q "deprecated bare-name search" compat.out || {
    echo "FAIL: the bare-name fallback must announce itself"; cat compat.out; exit 1; }
grep -q "compat.gadget" compat.out || {
    echo "FAIL: the warning must name the exact replacement"; cat compat.out; exit 1; }
grep -q "\[dependencies.compat\]" compat.out || {
    echo "FAIL: the warning must show the manifest edit"; cat compat.out; exit 1; }

# The explicit spelling resolves the same package and says nothing: a user who
# has already migrated must not be nagged.
write_manifest '[dependencies.compat]
gadget = "2.0.0"'
"$MCPP" build > compat-explicit.out 2>&1 || true
if grep -q "no package found for exact selector" compat-explicit.out; then
    echo "FAIL: explicit compat.gadget must resolve"; cat compat-explicit.out; exit 1
fi
if grep -q "deprecated bare-name search" compat-explicit.out; then
    echo "FAIL: an already-exact selector must not warn"
    cat compat-explicit.out; exit 1
fi

# ── 6. the exit ramp is not a wildcard ──────────────────────────────
# The fallback reaches `compat` and the namespace-less rung. It must NOT reach
# a third-party namespace — section 1 already proved bare `widget` fails, and
# it must keep failing now that a fallback exists at all. Stating the exact
# namespace is likewise not eligible: it is an identity, not an omission.
write_manifest '[dependencies.mcpplibs]
gadget = "2.0.0"'
if "$MCPP" build > explicit-default.out 2>&1; then
    echo "FAIL: mcpplibs.gadget states an identity and must not reach compat"
    cat explicit-default.out; exit 1
fi
grep -q "no package found for exact selector" explicit-default.out || {
    echo "FAIL: expected the explicit not-found error"
    cat explicit-default.out; exit 1; }

# ── 7. `mcpp add` did-you-mean carries versions too (#487) ──────────
# The same suggestion text serves the build path (sections 1-2) and the add
# path. Requesting a gadget under `acme` — a readable index that does not
# serve it — must fail AND point at compat.gadget with its published version,
# straight from the fixture's xpm table.
write_manifest ''
if MCPP_OFFLINE=1 "$MCPP" add acme.gadget@9.9.9 > add-hint.out 2>&1; then
    echo "FAIL: adding a package the acme index does not serve must fail"
    cat add-hint.out; exit 1
fi
grep -q "compat.gadget (2.0.0)" add-hint.out || {
    echo "FAIL: add hint must name compat.gadget with its published version"
    cat add-hint.out; exit 1; }
# And the manifest must NOT have been mutated by the failed add.
if grep -q "gadget" mcpp.toml; then
    echo "FAIL: a rejected add must not write the dependency"; cat mcpp.toml; exit 1
fi

echo "PASS 162_bare_name_namespace_scope"
