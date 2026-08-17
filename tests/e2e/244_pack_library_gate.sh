#!/usr/bin/env bash
# requires:
# (no capability: a library package is claimed to work on every target, so this
#  test has to RUN on every platform. `# requires: gcc` would have skipped it on
#  macOS and Windows — Apple Clang is not the gcc capability — leaving the claim
#  unverified while the suite stayed green.)
# 244_pack_library_gate.sh — the three refusals a prebuilt package must make.
#
# THE FIRST ONE IS WHY THIS FEATURE HAS A GATE AT ALL. Measured before it
# existed, on a real build: change one line of a shipped interface — swap two
# `int` members of a struct, which the Itanium ABI does not mangle — and the
# consumer compiles, links, runs, and prints transposed data. Exit code 0. No
# diagnostic at any stage, from any tool.
#
#   1. an edited interface           → refuse (the silent-wrong-data case)
#   2. an artifact for another toolchain → refuse, and LIST the tags it has
#   3. `mcpp build` inside the package   → refuse (it would "succeed" emptily)
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
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
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
EOF

cd mathkit
"$MCPP" pack mathkit > pack.log 2>&1 || { cat pack.log; echo "pack failed"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
# The manifest below is FILE CONTENT: on Git Bash a shell-spelled
# /tmp/... path is read by a native mcpp.exe as "root of the current
# drive". host_path is the conversion (tests/e2e/_host_path.sh).
PKG_HOST="$(host_path "$pkg")"
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

# Baseline: an untouched package builds. Without this the refusals below would
# be indistinguishable from "the package never worked".
( cd app && "$MCPP" run > ok.log 2>&1 ) || { cat app/ok.log; echo "baseline failed"; exit 1; }
grep -q 'ok=42' app/ok.log || { cat app/ok.log; echo "baseline wrong answer"; exit 1; }

# ── 1. an edited interface is refused ──────────────────────────────────
cp "$pkg/interface/mathkit.cppm" "$TMP/interface.bak"
printf '\n// tampered\n' >> "$pkg/interface/mathkit.cppm"
rm -rf app/target
if ( cd app && "$MCPP" build > tamper.log 2>&1 ); then
    cat app/tamper.log
    echo "FAIL: an edited interface built anyway — this is the silent-wrong-data case"
    exit 1
fi
grep -q 'does not match what was packaged' app/tamper.log || {
    cat app/tamper.log; echo "refused, but not for the interface digest"; exit 1; }
cp "$TMP/interface.bak" "$pkg/interface/mathkit.cppm"

# ── 2. a foreign toolchain tag is refused, and the real tags are shown ──
cp "$pkg/mcpp.toml" "$TMP/manifest.bak"
sed -i.bak 's/-gcc\([0-9][0-9]*\)-/-gcc999-/' "$pkg/mcpp.toml"
rm -rf app/target
if ( cd app && "$MCPP" build > tag.log 2>&1 ); then
    cat app/tag.log
    echo "FAIL: a package built for another compiler was accepted"
    exit 1
fi
grep -q 'no prebuilt artifact matches this toolchain' app/tag.log || {
    cat app/tag.log; echo "refused, but not for the abi tag"; exit 1; }
# The diagnostic has to say what IS available — a refusal the reader cannot act
# on sends them looking for a package that is right in front of them.
grep -q 'published tags' app/tag.log || {
    cat app/tag.log; echo "the refusal did not list the published tags"; exit 1; }
grep -q 'gcc999' app/tag.log || {
    cat app/tag.log; echo "the refusal did not name the tag it found"; exit 1; }
cp "$TMP/manifest.bak" "$pkg/mcpp.toml"

# ── 3. building INSIDE the package is refused ──────────────────────────
if ( cd "$pkg" && "$MCPP" build > "$TMP/inside.log" 2>&1 ); then
    cat "$TMP/inside.log"
    echo "FAIL: building inside a distribution package 'succeeded' — it compiles"
    echo "      declarations, links nothing, and reports Finished"
    exit 1
fi
grep -q 'distribution package produced by' "$TMP/inside.log" || {
    cat "$TMP/inside.log"; echo "refused, but not as a distribution package"; exit 1; }

# ── and the restored package still builds ──────────────────────────────
rm -rf app/target
( cd app && "$MCPP" run > final.log 2>&1 ) || { cat app/final.log; echo "restore failed"; exit 1; }
grep -q 'ok=42' app/final.log || { cat app/final.log; echo "restore wrong answer"; exit 1; }

echo "PASS: interface tamper, tag mismatch, and in-package build are all refused"
