#!/usr/bin/env bash
# requires:
# (no capability: this must hold on every target.)
#
# ⚠️ This test is also the regression for a scanner bug it uncovered. An
# IMPLEMENTATION PARTITION (`module M:part;`, no `export`) had no coverage
# anywhere in mcpp, and the scanner recorded it as "requires M:part, provides
# nothing" — so the file required its own name, the graph held no edge from the
# unit importing the partition to the unit defining it, and build order was
# unconstrained. GCC and macOS clang recovered via their own scan; Windows clang
# failed with `failed to read compiled module`. The scanner now models it, and
# the warning it used to print on every platform ("imported but not provided in
# this build") is gone.
# 243_pack_library_interface_closure.sh — what travels is the module closure of
# the published root, and the archive keeps exactly what the closure does not.
#
# Two asymmetric failures are pinned here, and only one of them is loud in the
# wild:
#
#   * publishing TOO LITTLE fails in the consumer's compile, naming the module;
#   * publishing TOO MUCH silently ships a closed-source implementation
#     partition's SOURCE. Nothing fails. That is what this test is for.
#
# The archive side is the same closure used the other way round, and getting it
# wrong was measured: dropping every `.m.o` also drops the implementation
# partition's object, and every target then fails to link with an undefined
# reference nowhere near its cause.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export import :api;
EOF
cat > mathkit/src/api.cppm <<'EOF'
export module mathkit:api;
export namespace mk { int answer(); }
EOF
# An implementation partition: it produces a BMI and a `.m.o`, and its source
# is the thing a closed-source publisher must not ship.
cat > mathkit/src/secret.cppm <<'EOF'
module mathkit:secret;
namespace mk { int secret_bias() { return 40; } }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk {
int secret_bias();
int answer() { return secret_bias() + 2; }
}
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
EOF

cd mathkit
"$MCPP" pack mathkit > pack.log 2>&1 || { cat pack.log; echo "pack failed"; exit 1; }
pkg="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
# The manifest below is FILE CONTENT: on Git Bash a shell-spelled
# /tmp/... path is read by a native mcpp.exe as "root of the current
# drive". host_path is the conversion (tests/e2e/_host_path.sh).
PKG_HOST="$(host_path "$TMP/mathkit/$pkg")"

# ── the confidentiality criterion ──────────────────────────────────────
[[ -f "$pkg/interface/mathkit.cppm" ]] || { echo "root interface not published"; exit 1; }
[[ -f "$pkg/interface/api.cppm" ]]     || { echo "interface partition not published"; exit 1; }
[[ ! -e "$pkg/interface/secret.cppm" ]] || {
    echo "LEAK: the implementation partition's source was published"; exit 1; }
grep -RIl 'secret_bias' "$pkg/interface" "$pkg/include" 2>/dev/null | grep -q . && {
    echo "LEAK: implementation source text found in the published interface"; exit 1; }

# ── the archive criterion: published objects out, everything else in ────
# A functional probe, not `command -v ar`: on macOS `ar` resolves to an xlings
# shim that reports "not installed" and exits non-zero, which under `set -e`
# kills the test instead of skipping the inspection.
archive="$(find "$pkg/lib" -name 'libmathkit.a' | head -1)"
if [[ -n "$archive" ]] && members="$(ar t "$archive" 2>/dev/null)"; then
    echo "$members" | grep -q 'secret.m.o' || {
        echo "the implementation partition's OBJECT was dropped; nothing would link"
        echo "$members"; exit 1; }
    echo "$members" | grep -q 'mathkit.m.o' && {
        echo "a published interface unit's object is still in the archive"
        echo "$members"; exit 1; }
    echo "$members" | grep -q 'api.m.o' && {
        echo "a published interface unit's object is still in the archive"
        echo "$members"; exit 1; }
fi

# ── the hazard case: an interface that reaches the partition ────────────
#
# Legal, and the consumer needs that source to build the BMI at all — so it is
# published rather than refused. But for a closed-source library it is the one
# outcome nobody wants by accident, and `import :secret;` in an interface looks
# like any other import, so `mcpp pack` has to say it.
cd "$TMP/mathkit"
cp src/mathkit.cppm "$TMP/root.bak"
printf 'import :secret;\n' >> src/mathkit.cppm
rm -rf target
"$MCPP" pack mathkit > hazard.log 2>&1 || { cat hazard.log; echo "hazard pack failed"; exit 1; }
grep -q 'secret.cppm is an implementation partition' hazard.log || {
    cat hazard.log
    echo "FAIL: publishing an implementation partition's source was not reported"
    exit 1; }
hazpkg="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
[[ -e "$hazpkg/interface/secret.cppm" ]] || {
    echo "the warning fired but the source was not actually published"; exit 1; }
cp "$TMP/root.bak" src/mathkit.cppm
rm -rf target
"$MCPP" pack mathkit > repack.log 2>&1 || { cat repack.log; echo "repack failed"; exit 1; }
pkg="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
[[ ! -e "$pkg/interface/secret.cppm" ]] || { echo "restore did not withhold it again"; exit 1; }
PKG_HOST="$(host_path "$TMP/mathkit/$pkg")"

# ── and it still links and runs ─────────────────────────────────────────
cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = { path = "$PKG_HOST" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
( cd app && "$MCPP" run > run.log 2>&1 ) || { cat app/run.log; echo "consumer failed"; exit 1; }
grep -q 'ok=42' app/run.log || { cat app/run.log; echo "wrong answer"; exit 1; }

echo "PASS: the closure decides both what is published and what is dropped"
