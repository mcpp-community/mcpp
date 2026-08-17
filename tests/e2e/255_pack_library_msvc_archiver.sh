#!/usr/bin/env bash
# requires: msvc
# 255_pack_library_msvc_archiver.sh — packing a library with the MSVC toolchain
# actually runs `lib.exe /REMOVE:`.
#
# WHY THIS EXISTS. `mcpp pack` deletes the published interface's objects from the
# archive before shipping it: the consumer compiles those sources itself, so
# leaving the objects in gives it two definitions of each published module's
# initialiser, resolved by link order. The two archivers disagree about how to
# say that, in both directions:
#
#   ar        one verb, then the archive, then every member
#               ar d libmathkit.a mathkit.m.o api.m.o
#   lib.exe   one flag PER member, and the archive comes LAST
#               lib.exe /REMOVE:mathkit.m.o /REMOVE:api.m.o mathkit.lib
#
# The packer originally assumed `ar` syntax everywhere. Nothing caught it,
# because mcpp's own Windows CI builds with clang and archives with `llvm-ar`,
# which takes the GNU spelling — so the MSVC branch has never executed in any
# job on any platform. test_pack_archive_remove.cpp pins the string; only a real
# `lib.exe` can tell whether the string is right.
#
# WHY PACK SUCCEEDING *IS* THE ASSERTION. A wrong spelling does not degrade
# quietly here: run_library_pack refuses, quoting the command and the archiver's
# output, because shipping the objects would be the silent outcome. So this test
# does not need to inspect the archive to know the branch worked — but it does
# have to prove the branch was TAKEN, and that is what the two checks below are
# for: a `.lib` (so the family is MSVC, not llvm-ar's `.a`) and a non-empty
# published interface (so there was something to remove at all).
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
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF
# msvc@system pinned, not defaulted: Windows' default toolchain is clang
# targeting the MSVC ABI, and it archives with llvm-ar — i.e. taking the default
# here would exercise the GNU branch and pass while proving nothing.
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
[toolchain]
windows = "msvc@system"
EOF

cd mathkit
"$MCPP" pack mathkit > pack.log 2>&1 || {
    cat pack.log
    echo "FAIL: packing with msvc@system failed."
    echo "      If the message above is 'cannot drop published interface objects',"
    echo "      the lib.exe spelling is wrong — /REMOVE: takes one flag per member"
    echo "      and the archive comes LAST (src/pack/library.cppm)."
    exit 1; }

pkg="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
[[ -n "$pkg" ]] || { cat pack.log; echo "FAIL: no package directory"; exit 1; }

# The branch was taken: MSVC names archives `<name>.lib` with no `lib` prefix,
# so finding one means `archive_tool` resolved to lib.exe rather than llvm-ar.
archive="$(find "$pkg/lib" -type f -name 'mathkit.lib' | head -1)"
[[ -n "$archive" ]] || {
    find "$pkg/lib" -type f
    echo "FAIL: no mathkit.lib in the package — this ran with a GNU-spelling"
    echo "      archiver, so the lib.exe path was never exercised."
    exit 1; }

# And there was something to remove: an empty interface list means dropObjects
# is empty and the removal is skipped entirely.
[[ -f "$pkg/interface/mathkit.cppm" && -f "$pkg/interface/api.cppm" ]] || {
    ls -R "$pkg"
    echo "FAIL: nothing was published as source, so nothing had to be removed"
    exit 1; }

# Best-effort member inspection. `lib.exe` lives in the VC toolset, not on PATH,
# so this is a bonus rather than the criterion — mcpp resolves it internally and
# the pack above already depended on it working.
if command -v lib &>/dev/null && members="$(lib /nologo /LIST "$(host_path "$archive")" 2>/dev/null)"; then
    echo "$members" | grep -qi 'impl' || {
        echo "$members"
        echo "FAIL: the implementation object is gone — nothing would link"
        exit 1; }
    echo "$members" | grep -qi 'api\.m\.obj' && {
        echo "$members"
        echo "FAIL: a published interface unit's object is still in the archive"
        exit 1; }
    echo "  (verified against lib /LIST)"
fi

# The end-to-end criterion: a consumer builds and runs against it.
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
mathkit = { path = "$(host_path "$TMP/mathkit/$pkg")" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
[toolchain]
windows = "msvc@system"
EOF
( cd app && "$MCPP" run > run.log 2>&1 ) || { cat app/run.log; echo "consumer failed"; exit 1; }
grep -q 'ok=42' app/run.log || { cat app/run.log; echo "wrong answer"; exit 1; }

echo "PASS: lib.exe /REMOVE: really removes, and the package links under MSVC"
