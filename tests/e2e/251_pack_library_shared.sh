#!/usr/bin/env bash
# requires:
# 251_pack_library_shared.sh — a `kind = "shared"` package is either produced
# CORRECTLY or refused CLEARLY. Never silently wrong.
#
# Both halves, because testing only the working one cannot tell "the gate is
# handled" from "there is no gate". Which half applies is now a property of the
# TARGET, not of "is it Linux":
#
#   ELF, Mach-O, PE/MinGW   produced — the package carries every name the
#                           platform needs to link it and to find it later
#   PE/MSVC                 refused — MSVC exports nothing from a DLL without
#                           `__declspec(dllexport)`, so the import library would
#                           be empty and consumers would fail with unresolved
#                           externals for symbols visibly present in the objects
#
# Windows' default toolchain here is clang on the MSVC ABI, so this file sees the
# refusal there. 258 pins that refusal in detail, 259 pins Mach-O relocatability,
# 257 pins the PE/MinGW path; what this one adds is that the two outcomes are
# reachable from the same fixture and the same command.
#
# A shared library is LINKED by `lib<target>.so` and FOUND at run time by its
# SONAME, and those are two different filenames. The first version of this
# packer shipped only the built file: the consumer linked, and then mcpp's own
# runtime-closure check reported
#
#   libmathkit.so.1 not found on the search path this artifact will actually use
#
# which is the good outcome only because that check exists. Without it the
# program would have failed to start with a loader error naming a file the user
# never asked for.
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
[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
EOF

cd mathkit

# ── Windows: the MSVC ABI is refused, and says what to do instead ──────
if [[ "$(uname -s)" != "Linux" && "$(uname -s)" != "Darwin" ]]; then
    if "$MCPP" pack mathkit-shared > refuse.log 2>&1; then
        cat refuse.log
        echo "FAIL: a shared library was packed for the MSVC ABI. Its import"
        echo "      library has no exports, so consumers fail with unresolved"
        echo "      externals naming symbols that are in the objects."
        exit 1
    fi
    grep -qi 'dllexport' refuse.log || {
        cat refuse.log
        echo "FAIL: it refused, but not for the export reason — 'shared libraries"
        echo "      are not supported here' does not tell the reader what to change."
        exit 1; }
    grep -qi 'windows-gnu' refuse.log || {
        cat refuse.log
        echo "FAIL: the refusal names no way forward. MinGW auto-exports."
        exit 1; }
    echo "PASS: a shared library package is refused on the MSVC ABI, with the reason"
    exit 0
fi

# ── macOS: it is produced, and the deep claims live in 259 ──────────────
if [[ "$(uname -s)" == "Darwin" ]]; then
    "$MCPP" pack mathkit-shared > pack.log 2>&1 \
        || { cat pack.log; echo "FAIL: shared pack failed on Mach-O"; exit 1; }
    macpkg="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
    [[ -n "$(find "$macpkg" -name 'libmathkit-shared.dylib' | head -1)" ]] || {
        find "$macpkg" \( -type f -o -type l \)
        echo "FAIL: no .dylib in the package"; exit 1; }
    echo "PASS: a shared library package is produced on Mach-O"
    exit 0
fi

# ── the ELF side: produce it, with both names ──────────────────────────
"$MCPP" pack mathkit-shared > pack.log 2>&1 || { cat pack.log; echo "shared pack failed"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
# The manifest below is FILE CONTENT: on Git Bash a shell-spelled
# /tmp/... path is read by a native mcpp.exe as "root of the current
# drive". host_path is the conversion (tests/e2e/_host_path.sh).
PKG_HOST="$(host_path "$pkg")"

libdir="$(dirname "$(find "$pkg/lib" -name 'libmathkit-shared.so' | head -1)")"
[[ -n "$libdir" ]] || { echo "no .so in the package"; find "$pkg" -type f; exit 1; }
# Both names. The SONAME one may be a symlink or a copy — either is fine, its
# absence is not.
[[ -e "$libdir/libmathkit.so.1" ]] || {
    echo "FAIL: the package does not carry the SONAME the loader will ask for"
    ls -l "$libdir"; exit 1; }

# The manifest declares it as a shared library and gives a runtime search dir:
# link_library_dirs is not rpath, and a shared package needs both.
grep -q 'role *= *"shared-library"' "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"; echo "artifact is not recorded as a shared library"; exit 1; }
grep -q 'runtime_search_dirs' "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"; echo "no runtime_search_dirs — the consumer could not find it"; exit 1; }

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

# `run`, not `build`: linking proves nothing here — the whole point is that the
# process starts and the loader resolves the SONAME.
( cd app && "$MCPP" run > run.log 2>&1 ) || { cat app/run.log; echo "consumer failed to run"; exit 1; }
grep -q 'ok=42' app/run.log || { cat app/run.log; echo "wrong answer"; exit 1; }

exe="$(find app/target -name app -type f | head -1)"
readelf -d "$exe" 2>/dev/null | grep -q 'libmathkit.so.1' || {
    readelf -d "$exe"; echo "the consumer does not NEED the soname"; exit 1; }

echo "PASS: a shared library package carries both names and the consumer starts"
