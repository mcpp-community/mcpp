#!/usr/bin/env bash
# requires:
# (no capability: nothing here is toolchain-specific.)
#
# 261_pack_follows_the_module_extension.sh — `mcpp pack` follows the project's
# module extension on its own, and the package it writes states that extension
# for its consumers.
#
# The principle: a project says `module_extensions = [".ixx"]` ONCE. Packing it
# and consuming the package must both work with nothing further declared on
# either side. Anything else makes the knob a knob plus two things to remember.
#
# ⚠️ TWO DEFECTS SAT BEHIND THIS, and the first was silent in both directions.
#
# 1. The lib-root convention hard-coded `.cppm`: `src/<tail>.cppm`. For an
#    `.ixx` project that file does not exist, so the closure started nowhere:
#
#        $ mcpp pack mathkit
#             Interface (headers only)      ← the module interface, gone
#              Withheld (nothing)
#          Packed …-x86_64-linux-gnu        ← the C-SURFACE tag
#
#    Both halves wrong without a word. No consumer could `import mathkit`; and
#    an empty published set is precisely how the packer recognises a C surface,
#    so the package ALSO stopped constraining the C++ ABI and the compatibility
#    gate stopped checking compiler and stdlib. A package that claims less than
#    it should is the failure this whole design exists to prevent.
#
# 2. The generated manifest listed `sources = ["interface/mathkit.ixx"]` and did
#    not say what an `.ixx` is, so the consumer was refused by the classifier.
#    Now the package declares it — computed from the published FILES, so it
#    cannot disagree with `sources`.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.ixx <<'EOF'
export module mathkit;
export import :api;
EOF
cat > mathkit/src/api.ixx <<'EOF'
export module mathkit:api;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.ixx", "src/*.cpp"]
module_extensions = [".ixx"]
[targets.mathkit]
kind = "lib"
EOF

cd mathkit
"$MCPP" pack mathkit --format dir > pack.log 2>&1 || { cat pack.log; echo "pack failed"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"

# ── the closure actually started ────────────────────────────────────────
[[ -f "$pkg/interface/mathkit.ixx" && -f "$pkg/interface/api.ixx" ]] || {
    find "$pkg" -type f
    cat pack.log
    echo "FAIL: the module interface was not published. 'Interface (headers only)'"
    echo "      in the log above means the lib root resolved to a file that does"
    echo "      not exist — the convention looked for .cppm."
    exit 1; }
[[ ! -e "$pkg/interface/impl.cpp" ]] || { echo "FAIL: the implementation unit was published"; exit 1; }

# ── and the package is tagged as the C++ surface it is ──────────────────
#
# Asserted separately because it fails INDEPENDENTLY: an empty published set is
# how a C surface is recognised, so losing the interface also silently downgrades
# the compatibility tag, and a package that constrains nothing is accepted by
# every toolchain.
grep -qE '^abi +=.*-c\+\+[0-9]+"' "$pkg/mcpp.toml" || {
    grep -n 'abi' "$pkg/mcpp.toml"
    echo "FAIL: the leg carries a C-surface tag. A package that publishes a C++"
    echo "      module interface constrains the C++ ABI, and this one now says"
    echo "      it does not — the gate will accept any compiler."
    exit 1; }

# ── the package tells consumers what an .ixx is ─────────────────────────
grep -q 'module_extensions' "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"
    echo "FAIL: the generated manifest lists .ixx sources without declaring the"
    echo "      extension, so every consumer is refused by the classifier."
    exit 1; }

# ── a consumer that declares NOTHING builds and runs ────────────────────
PKG_HOST="$(host_path "$pkg")"
cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("pack-ixx=%d\n", mk::answer()); return 0; }
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
( cd app && "$MCPP" run > run.log 2>&1 ) || {
    cat app/run.log
    echo "FAIL: the consumer had to be told about .ixx. The package should carry it."
    exit 1; }
grep -q 'pack-ixx=42' app/run.log || { cat app/run.log; echo "FAIL: wrong answer"; exit 1; }

# ── a .cppm package's manifest is unchanged ─────────────────────────────
#
# The new key must appear only when the published set needs it, or every
# existing package's manifest changes shape for no reason.
cd "$TMP"
mkdir -p plain/src
cat > plain/src/plain.cppm <<'EOF'
export module plain;
export namespace pl { int v() { return 1; } }
EOF
cat > plain/mcpp.toml <<'EOF'
[package]
name    = "plain"
version = "0.1.0"
[build]
sources = ["src/*.cppm"]
[targets.plain]
kind = "lib"
EOF
( cd plain && "$MCPP" pack plain --format dir > pack.log 2>&1 ) || {
    cat plain/pack.log; echo "plain pack failed"; exit 1; }
plainpkg="$(find plain/target/dist -maxdepth 1 -type d -name 'plain-0.1.0-*' | head -1)"
grep -q 'module_extensions' "$plainpkg/mcpp.toml" && {
    cat "$plainpkg/mcpp.toml"
    echo "FAIL: a .cppm-only package gained a module_extensions key it does not need"
    exit 1; }

echo "PASS: pack follows the project's module extension, and the package carries it"
