#!/usr/bin/env bash
# requires: msvc
# 258_shared_library_msvc_auto_def.sh — a DLL on the MSVC ABI, exported without
# a single `__declspec(dllexport)` in the source.
#
# MSVC exports nothing from a DLL unless the source says `__declspec(dllexport)`
# or a `.def` lists the symbols. Without either, the import library comes out
# empty and every consumer fails with unresolved externals for symbols that are
# plainly in the object files — a diagnostic pointing nowhere near its cause.
# MinGW's linker auto-exports and hides the whole problem; lld-link's MSVC
# flavour does not, deliberately, because PE caps exports at 65535.
#
# So mcpp writes the `.def` from the objects, which is what CMake's
# `WINDOWS_EXPORT_ALL_SYMBOLS` has done since 3.4. The filtering rules are unit
# tested against synthetic and real COFF (tests/unit/test_coff_exports.cpp); what
# only a real link can answer is whether link.exe accepts the result and whether
# a consumer can then resolve a symbol through it. That is this test.
#
# ⚠️ THE TWO LIMITS ARE ASSERTED, NOT ASSUMED. Auto-export cannot make a data
# symbol readable without `__declspec(dllimport)` on the consumer's declaration,
# and CMake documents the same limit for the same mechanism. The test exercises a
# FUNCTION across the boundary, which is what auto-export does cover, and says so
# — a test that quietly used only functions would read as if the limit did not
# exist.
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
# No dllexport anywhere. That is the point.
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
kind   = "shared"
[toolchain]
windows = "msvc@system"
EOF

cd mathkit
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: a kind=\"shared\" target did not build on the MSVC ABI."
    echo "      If the message mentions dllexport, the old refusal is still in place."
    exit 1; }

# ── the .def was generated, and it is a build-graph node ────────────────
nj="$(find target -name build.ninja | head -1)"
grep -qE "^build .*mathkit\.def : coff_def " "$nj" || {
    grep -n 'def' "$nj" | head
    echo "FAIL: no coff_def edge. The .def has to be produced from the same"
    echo "      objects the link consumes, or the exported surface can drift"
    echo "      from what was compiled."
    exit 1; }
def="$(find target -name 'mathkit.def' | head -1)"
[[ -n "$def" ]] || { echo "FAIL: no .def was written"; exit 1; }
grep -q '^EXPORTS' "$def" || { cat "$def"; echo "FAIL: malformed .def"; exit 1; }
# Non-empty is the criterion: an empty EXPORTS section links fine and produces
# exactly the import library this whole mechanism exists to avoid.
[[ "$(grep -cvE '^(LIBRARY|EXPORTS|$)' "$def")" -gt 0 ]] || {
    cat "$def"
    echo "FAIL: the .def exports nothing, so the import library is empty and"
    echo "      every consumer will fail with unresolved externals."
    exit 1; }

# ── the DLL and its import library both exist ───────────────────────────
[[ -n "$(find target -name 'mathkit.dll' | head -1)" ]] || { echo "FAIL: no DLL"; exit 1; }
[[ -n "$(find target -name 'mathkit.lib' | head -1)" ]] || {
    find target -name '*.lib'
    echo "FAIL: no import library beside the DLL"; exit 1; }

# ── and a consumer resolves a function through it ───────────────────────
#
# A function, deliberately: auto-export covers code. Exported DATA additionally
# needs `__declspec(dllimport)` on the consumer's declaration — a limit of the
# mechanism, documented in docs/12, not a defect of this test.
"$MCPP" pack mathkit > pack.log 2>&1 || { cat pack.log; echo "FAIL: pack"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
PKG_HOST="$(host_path "$pkg")"

cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("msvc-dll=%d\n", mk::answer()); return 0; }
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
[toolchain]
windows = "msvc@system"
EOF
( cd app && "$MCPP" run > run.log 2>&1 ) || {
    cat app/run.log
    echo "FAIL: the consumer could not link or start against the MSVC DLL."
    echo "      'unresolved external symbol' here means the .def did not reach"
    echo "      the link, or exported the wrong names."
    exit 1; }
grep -q 'msvc-dll=42' app/run.log || { cat app/run.log; echo "FAIL: wrong answer"; exit 1; }

echo "PASS: MSVC produces an exporting DLL with no dllexport in the source"
