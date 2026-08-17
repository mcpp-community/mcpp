#!/usr/bin/env bash
# requires: gcc mingw-cross
# 248_pack_library_fat_pe_leg.sh — a fat package whose legs cross an OS
# boundary, not just a libc one.
#
# 245 covers the fat-package MECHANISM with gnu + musl, because CI warms both
# and that test must never skip. This one adds the leg that changes binary
# format: `x86_64-windows-gnu` is PE, and its artifact naming follows the
# ENVIRONMENT rather than the OS — MinGW writes `libfoo.a` where MSVC would
# write `foo.lib`, which is exactly why `lib/` is keyed by triple and not by OS.
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
"$MCPP" pack mathkit \
    --target x86_64-linux-gnu \
    --target x86_64-windows-gnu > pack.log 2>&1 \
    || { cat pack.log; echo "cross-OS fat pack failed"; exit 1; }

pkg="$TMP/mathkit/target/dist/mathkit-0.1.0"
# The manifest below is FILE CONTENT: on Git Bash a shell-spelled
# /tmp/... path is read by a native mcpp.exe as "root of the current
# drive". host_path is the conversion (tests/e2e/_host_path.sh).
PKG_HOST="$(host_path "$pkg")"
for t in x86_64-linux-gnu x86_64-windows-gnu; do
    [[ -n "$(find "$pkg/lib/$t" -name 'libmathkit.a' | head -1)" ]] || {
        echo "no artifact for $t"; find "$pkg" -type f; exit 1; }
done

# Each leg records the triple it was built for. Two legs, two distinct tags —
# if the tag were taken from the compiler's own `-dumpmachine` instead of
# mcpp's canonical vocabulary, the Windows one would say `x86_64-w64-mingw32`
# and disagree with the cfg() block selecting it.
grep -q 'abi *= *"x86_64-linux-gnu-'   "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"; echo "no linux-gnu tag"; exit 1; }
grep -q 'abi *= *"x86_64-windows-gnu-' "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"; echo "no windows-gnu tag (compiler spelling leaked?)"; exit 1; }

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

# The PE consumer must link the PE leg and produce a real PE image.
( cd app && "$MCPP" build --target x86_64-windows-gnu > win.log 2>&1 ) \
    || { cat app/win.log; echo "PE consumer failed"; exit 1; }
exe="$(find app/target/x86_64-windows-gnu -name 'app.exe' | head -1)"
[[ -n "$exe" ]] || { echo "no .exe produced"; exit 1; }
file "$exe" | grep -q 'PE32+' || { file "$exe"; echo "not a PE image"; exit 1; }

nj="$(find app/target/x86_64-windows-gnu -name build.ninja | head -1)"
grep -o "dist/mathkit-0.1.0/lib/[a-z0-9_-]*" "$nj" | sort -u > "$TMP/legs"
[[ "$(wc -l < "$TMP/legs")" -eq 1 ]] || { echo "more than one leg:"; cat "$TMP/legs"; exit 1; }
grep -q 'lib/x86_64-windows-gnu$' "$TMP/legs" || {
    echo "the PE build did not pick the PE leg:"; cat "$TMP/legs"; exit 1; }

echo "PASS: a fat package crosses an OS boundary and each build picks its own leg"
