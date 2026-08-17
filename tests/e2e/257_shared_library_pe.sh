#!/usr/bin/env bash
# requires: mingw-cross wine
# 257_shared_library_pe.sh — `kind = "shared"` on PE: a DLL, its import library,
# a package carrying both, and a consumer that links and RUNS.
#
# Until now this was Linux-only: make_plan refused every non-Linux target, native
# or cross. So this test is a NEW CAPABILITY, not a repaired hole — an earlier
# draft of this header claimed the guard was inert on native builds because
# `targetTriple` would be empty there, and that is wrong: it is filled from the
# compiler's own -dumpmachine, and resolution.json records `x86_64-linux-gnu` for
# a plain `mcpp build`.
#
# What was actually missing on PE was the IMPORT LIBRARY. A PE shared library is
# two files: the `.dll` the loader opens, and an archive of stubs the linker
# consumes. mcpp wrote only the first, and consumers linked the `.dll` directly —
# which mingw's ld tolerates and no other linker does. So the tolerant case was
# hiding the broken one.
#
# Four things are asserted, in the order they can fail:
#   1. the build writes BOTH files
#   2. `mcpp pack` ships both, and points consumers at the import library
#   3. a consumer of the PACKAGE links, gets the DLL deployed beside its exe,
#      and prints the right answer under wine
#   4. an MSVC-ABI shared target is still refused, and says why
#
# ⚠️ WINE IS EVIDENCE, NOT PROOF. Wine maps the filesystem differently (Z:) and
# has previously passed things a real Windows failed. The Windows-native halves
# of the same claims live in 242/255/256; what wine gives here is the loader
# actually resolving the DLL, which no Linux-hosted check can show.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TRIPLE=x86_64-windows-gnu

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
kind = "shared"
EOF

# ── 1. the build writes the DLL and the import library ──────────────────
cd mathkit
"$MCPP" build --target "$TRIPLE" > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
dll="$(find target -name 'libmathkit.dll' | head -1)"
imp="$(find target -name 'libmathkit.dll.a' | head -1)"
[[ -n "$dll" ]] || { find target -type f; echo "FAIL: no .dll"; exit 1; }
[[ -n "$imp" ]] || {
    find target -type f
    echo "FAIL: no import library. The .dll alone is a library only mingw's ld"
    echo "      will link, so the package would be unusable everywhere else."
    exit 1; }
# ⚠️ And no `-fPIC`. PE code is position independent by design, and clang
# targeting the MSVC ABI REJECTS the flag — `unsupported option '-fPIC' for
# target 'x86_64-pc-windows-msvc'` — killing the build in clang-scan-deps before
# anything compiles. The condition used to be the DIALECT rather than the target,
# and Windows' default toolchain is clang, which speaks the GNU dialect while
# targeting MSVC. Asserted here rather than only on Windows because this runs on
# every Linux CI pass through mingw-cross, and the flag is equally meaningless
# for a PE target whichever compiler emits it.
nj_pic="$(find target -name build.ninja | head -1)"
grep -q '\-fPIC' "$nj_pic" && {
    grep -n 'fPIC' "$nj_pic" | head -3
    echo "FAIL: -fPIC on a PE target. It means nothing here, and clang targeting"
    echo "      the MSVC ABI refuses it outright."
    exit 1; }

# It is a declared output of the link edge, not a side effect ninja knows nothing
# about — otherwise the consumer that links it has no producer and ninja stops
# with 'no known rule to make it'.
nj="$(find target -name build.ninja | head -1)"
grep -qE "^build bin/libmathkit\.dll \| bin/libmathkit\.dll\.a : " "$nj" || {
    grep -n 'libmathkit' "$nj"
    echo "FAIL: the import library is not an implicit output of the link edge"
    exit 1; }

# ── 2. the package carries both, and names the right one to link ────────
rm -rf target
"$MCPP" pack mathkit --target "$TRIPLE" --format dir > pack.log 2>&1 \
    || { cat pack.log; echo "pack failed"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0*' | head -1)"
[[ -f "$pkg/lib/$TRIPLE/libmathkit.dll" ]]   || { find "$pkg" -type f; echo "FAIL: no DLL in the package"; exit 1; }
[[ -f "$pkg/lib/$TRIPLE/libmathkit.dll.a" ]] || { find "$pkg" -type f; echo "FAIL: no import library in the package"; exit 1; }
# `-static` is what mcpp gives PE executables, and it puts ld in static-only
# mode where an import library is refused with a message that names neither the
# DLL nor `-static`. The emitted manifest has to switch modes for this one `-l`.
grep -q 'Bdynamic' "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"
    echo "FAIL: the shared leg's ldflags do not leave static-link mode, so a"
    echo "      consumer will fail with 'have you installed the static version'"
    exit 1; }

# ── 3. a consumer links it, gets the DLL, and runs ──────────────────────
cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
PKG_HOST="$(host_path "$pkg")"
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
( cd app && "$MCPP" build --target "$TRIPLE" > build.log 2>&1 ) || {
    cat app/build.log
    echo "FAIL: the consumer could not link against the packaged shared library."
    echo "      'multiple rules generate bin/libmathkit.dll' means a link unit was"
    echo "      created for a library that is already built."
    exit 1; }
exe="$(find app/target -name app.exe | head -1)"
[[ -n "$exe" ]] || { echo "FAIL: no app.exe"; exit 1; }
# PE has no rpath: the DLL has to BE there, or the process dies at load time
# with a status code and no message.
[[ -f "$(dirname "$exe")/libmathkit.dll" ]] || {
    ls "$(dirname "$exe")"
    echo "FAIL: the DLL was not deployed beside the exe — the program cannot start"
    exit 1; }
out="$(cd "$(dirname "$exe")" && WINEDEBUG=-all wine ./app.exe 2>&1 || true)"
grep -q 'ok=42' <<<"$out" || { echo "$out"; echo "FAIL: wrong answer from the PE consumer"; exit 1; }

# ── 4. windows-msvc is refused here, but by the TARGET gate ─────────────
#
# The MSVC-ABI shared library cannot be observed from a Linux host at all: this
# machine cannot serve `x86_64-windows-msvc`, so the target gate answers first
# and make_plan is never reached. That half lives in 258, under
# `# requires: msvc`, where a real link.exe can say whether the generated `.def`
# was accepted.
#
# What IS worth pinning here is that the refusal happens at all, and names the
# host rather than silently building an ELF: that is precisely what this used to
# do. `mcpp build --target x86_64-windows-msvc` on Linux resolved the native g++,
# wrote target/x86_64-linux-gnu/, and reported success.
cd "$TMP/mathkit"
if "$MCPP" build --target x86_64-windows-msvc > msvc.log 2>&1; then
    echo "FAIL: --target x86_64-windows-msvc SUCCEEDED on a host that cannot"
    echo "      serve it. Check target/: an ELF reported as a Windows build is"
    echo "      the failure mode the target vocabulary check exists to prevent."
    exit 1
fi
grep -q 'cannot be built on this host' msvc.log || {
    cat msvc.log
    echo "FAIL: refused, but not with the host-servability reason — the message"
    echo "      has to say which host can build it, or it is not actionable."
    exit 1; }

echo "PASS: PE shared libraries build, pack, link and run; unservable stays refused"
