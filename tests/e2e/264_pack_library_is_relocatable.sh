#!/usr/bin/env bash
# requires: pack python3
# 264_pack_library_is_relocatable.sh — a packed `kind = "shared"` library must
# not carry the BUILD MACHINE's loader search path (issue #460).
#
# WHAT WAS WRONG, AND WHY NOBODY SAW IT
#
# `run_library_pack` copied the built `.so` into the package and did nothing
# else, so it kept the DT_RUNPATH the link gave it:
#
#   <home>/registry/data/xpkgs/xim-x-glibc/2.44/lib64
#   : <home>/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64
#   : <home>/registry/subos/default/lib
#
# On another machine those directories do not exist, and — this is the part
# that makes it fatal rather than untidy — a shared object that carries ANY
# DT_RUNPATH makes the loader skip the whole inherited DT_RPATH chain when
# resolving that object's own dependencies. So the consumer's carefully
# computed DT_RPATH (payload + package dir + SubOS farm) is not consulted, and
# the program dies with
#
#   error while loading shared libraries: libstdc++.so.6: cannot open shared object file
#
# AND WHY `$ORIGIN` IS NOT THE FIX. Measured: a DT_RUNPATH of `$ORIGIN`, and
# a DT_RUNPATH of the empty string, both fail exactly the same way. It is the
# TAG's presence that disables inheritance, not its contents. The criterion is
# therefore "there is no tag", never "the tag is relative".
#
# AND WHY THIS TEST DELIBERATELY BREAKS THE PACKAGE HALF-WAY THROUGH.
# Asserting only that the fixed package runs cannot tell "the defect is fixed"
# from "this machine happens to satisfy the stale path" — which is precisely
# what made the pre-existing e2e (251) green throughout the bug's life: it
# consumes the package on the machine that built it, where those directories
# are right there. So the defect is put BACK with patchelf, the consumer is
# required to FAIL, and only then is it restored. A guard that cannot observe
# the defect is not a guard.
set -e
source "$(dirname "$0")/_host_path.sh"
source "$(dirname "$0")/_elf_tag.sh"

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
"$MCPP" pack mathkit-shared > pack.log 2>&1 || { cat pack.log; echo "FAIL: pack"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
PKG_HOST="$(host_path "$pkg")"
libdir="$pkg/lib/x86_64-linux-gnu"
[[ -d "$libdir" ]] || libdir="$(dirname "$(find "$pkg/lib" -name 'libmathkit-shared.so' | head -1)")"
so="$libdir/libmathkit-shared.so"
[[ -f "$so" ]] || { find "$pkg" -type f; echo "FAIL: no .so in the package"; exit 1; }

# ── 1. static: EVERY ELF in the package, and both of the .so's names ────────
#
# The SONAME alias is checked too, and that is not belt-and-braces: it is a
# symlink here, but on a filesystem where `create_symlink` fails the packer
# falls back to a COPY — and that copy used to be made from the BUILD TREE's
# file rather than from the staged one, i.e. from the unrelocated original,
# under the exact name the loader asks for.
fail=0
swept=0
while IFS= read -r obj; do
    head -c4 "$obj" 2>/dev/null | grep -q $'\x7fELF' || continue
    read -r form tag paths <<<"$(read_tag "$obj")"
    [[ "$form" == "NOT-ELF64" ]] && continue
    swept=$((swept + 1))
    printf '  %-34s %-14s %s %s\n' "$(basename "$obj")" "$form" "$tag" "$paths"
    if [[ "$tag" != "NONE" ]]; then
        echo "FAIL: a packaged library carries $tag ($paths)"
        echo "      A shipped .so must carry NO loader search path: any DT_RUNPATH —"
        echo "      including \$ORIGIN and the empty string — stops the consumer's"
        echo "      DT_RPATH from being inherited for this object's dependencies."
        fail=1
    fi
done < <(find "$libdir" \( -type f -o -type l \))
[[ "$swept" -ge 1 ]] || { echo "FAIL: swept no ELF — the test proved nothing"; exit 1; }
[[ "$fail" == "0" ]] || exit 1

# ── 2. a consumer of the package starts ────────────────────────────────────
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
( cd app && "$MCPP" build > build.log 2>&1 ) || { cat app/build.log; echo "FAIL: consumer build"; exit 1; }
exe="$(find app/target -path '*/bin/app' -type f | head -1)"
[[ -x "$exe" ]] || { echo "FAIL: no consumer binary"; exit 1; }

run_bare() { "$exe" 2>&1; }        # NOT `mcpp run`: no injected environment

out="$(run_bare)" && rc=0 || rc=$?
[[ "$rc" == "0" && "$out" == *"ok=42"* ]] || {
    echo "FAIL: the consumer does not start against the packed library"
    echo "      rc=$rc out=$out"; exit 1; }

# ── 3. put the defect BACK, and require the consumer to fail ───────────────
#
# `/nonexistent-machine/...` stands in for "the publisher's store, on somebody
# else's computer". Nothing is rebuilt: only the shipped library's dynamic
# section changes, which is exactly the difference between the two machines.
cp "$so" "$so.packed"
patchelf --set-rpath '/nonexistent-machine/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64' "$so"
out="$(run_bare)" && rc=0 || rc=$?
if [[ "$rc" == "0" ]]; then
    echo "FAIL: the consumer still ran with a stale RUNPATH restored on the library."
    echo "      That means this test cannot observe the defect it exists for —"
    echo "      the consumer is finding libstdc++ some other way, so a regression"
    echo "      in the packer would pass unnoticed."
    exit 1
fi
[[ "$out" == *"cannot open shared object file"* ]] || {
    echo "FAIL: the restored defect produced an unexpected failure: $out"; exit 1; }
echo "  defect restored → consumer fails as expected (rc=$rc)"

# ── 4. …and restore, so the pass is not an artifact of step 3 ──────────────
cp "$so.packed" "$so"
out="$(run_bare)" && rc=0 || rc=$?
[[ "$rc" == "0" && "$out" == *"ok=42"* ]] || {
    echo "FAIL: restoring the packed library did not restore the behaviour"
    echo "      rc=$rc out=$out"; exit 1; }

echo "PASS: a packed shared library carries no build-machine search path, and the guard can see the defect"
