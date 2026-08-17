#!/usr/bin/env bash
# requires:
# 251_pack_library_shared.sh — a `kind = "shared"` package is either produced
# CORRECTLY or refused CLEARLY. Never silently wrong.
#
# Both halves, because testing only the working one cannot tell "the gate is
# handled" from "there is no gate". Which half applies is now a property of the
# TARGET, not of "is it Linux":
#
#   ELF, Mach-O, PE/MinGW, PE/MSVC   produced — the package carries every name
#                                    the platform needs to link it and to find
#                                    it later
#
# Every format mcpp targets now produces one. MSVC was the last holdout, and
# what it was missing was never the linker: it exports nothing from a DLL
# without `__declspec(dllexport)` or a `.def`, so mcpp generates the `.def` from
# the objects (258 pins that path in detail, 259 pins Mach-O relocatability, 257
# the PE/MinGW one). What this file adds is that one fixture and one command
# produce a usable package on whichever platform it runs.
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

# ── Windows and macOS: it is produced; the deep claims live in 258 / 259 ──
if [[ "$(uname -s)" != "Linux" ]]; then
    "$MCPP" pack mathkit-shared > pack.log 2>&1 \
        || { cat pack.log; echo "FAIL: shared pack failed off ELF"; exit 1; }
    nonelf="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
    # `.dylib` on macOS, `.dll` on Windows — asked for by shape rather than by
    # `uname`, so the assertion is about the artifact and not about the host.
    [[ -n "$(find "$nonelf" \( -name '*.dylib' -o -name '*.dll' \) | head -1)" ]] || {
        find "$nonelf" \( -type f -o -type l \)
        echo "FAIL: no shared library in the package"; exit 1; }
    echo "PASS: a shared library package is produced off ELF too"
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
