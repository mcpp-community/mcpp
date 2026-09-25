#!/usr/bin/env bash
# check_unicode_paths.sh: mcpp#693 on Windows: a project in a directory whose
# name is not ASCII builds and runs on every toolchain row, and the files mcpp
# writes for other tools hold that name in UTF-8.
#
# THE SHAPE OF THE FAILURE THIS GUARDS. Up to mcpp 2026.9.25.1 mcpp held paths in
# the process ANSI code page. A name that page could spell (U+00E9 on code page
# 1252, every Chinese name on code page 936) reached compile_commands.json as
# ANSI bytes and failed every build with
#     error: internal: unhandled exception: [json.exception.type_error.316]
# and a name it could not spell failed with a narrowing exception. mcpp.exe now
# declares the UTF-8 code page, build programs are linked with the same
# declaration, and the response files of the MSVC tools begin with a byte order
# mark. Each of those is a separate way back to the failure, which is why every
# row runs in every directory.
#
# Rows: llvm (clang++, the Windows default), msvc (cl.exe) and mingw (gcc@16.1.0
# for x86_64-windows-gnu), each in an ASCII directory, a directory named
# `caf` + U+00E9 (inside code page 1252) and one named U+6D4B U+8BD5 (outside
# it). One more project runs a build.mcpp that reads its own directory from the
# environment and prints it back as an include directory: the round trip of a
# path through a build program. Names are built from bytes, so this file stays
# ASCII.
#
# Usage: MCPP=<mcpp.exe> bash .github/tools/check_unicode_paths.sh
set -uo pipefail

: "${MCPP:?set MCPP to the mcpp.exe under test}"
ROOT=$(mktemp -d)
CAFE="caf$(printf '\xc3\xa9')"
CJK="$(printf '\xe6\xb5\x8b\xe8\xaf\x95')"
CAFE_ANSI="caf$(printf '\xe9')"
failed=0

ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; failed=1; }

# write_project DIR TOOLCHAIN_LINES
write_project() {
    mkdir -p "$1/src"
    printf '[package]\nname    = "unicodeprobe"\nversion = "0.1.0"\n\n%b\n' "$2" > "$1/mcpp.toml"
    cat > "$1/src/main.cpp" <<'EOF'
import std;
int main() {
    std::println("unicode probe");
    return 0;
}
EOF
}

# The generated file holds the directory name in UTF-8, and never in the ANSI
# code page.
check_bytes() {
    local file=$1 name=$2 row=$3
    [ -f "$file" ] || { bad "$row: no $(basename "$file")"; return; }
    if ! LC_ALL=C grep -qF "$name" "$file"; then
        bad "$row: $(basename "$file") does not hold the directory name in UTF-8"
    fi
    if [ "$name" = "$CAFE" ] && LC_ALL=C grep -qF "$CAFE_ANSI" "$file"; then
        bad "$row: $(basename "$file") holds the ANSI spelling of the directory name"
    fi
}

rows=(
    'llvm|[toolchain]\nwindows = "llvm@20.1.7"|'
    'msvc|[toolchain]\nwindows = "msvc@system"|'
    'mingw|[toolchain]\ndefault = "gcc@16.1.0"|--target x86_64-windows-gnu'
)

for name in ascii "$CAFE" "$CJK"; do
    for row in "${rows[@]}"; do
        IFS='|' read -r rid tc args <<<"$row"
        dir="$ROOT/$name/$rid"
        label="$rid in '$name'"
        write_project "$dir" "$tc"
        cd "$dir" || { bad "$label: cannot enter the directory"; continue; }
        # shellcheck disable=SC2086
        if ! "$MCPP" build $args > build.log 2>&1; then
            bad "$label: the build failed"; tail -15 build.log | sed 's/^/          /'; continue
        fi
        # shellcheck disable=SC2086
        out=$("$MCPP" run $args 2>&1)
        if ! grep -q 'unicode probe' <<<"$out"; then
            bad "$label: the program did not run"; printf '%s\n' "$out" | tail -5 | sed 's/^/          /'
            continue
        fi
        if [ "$name" != ascii ]; then
            ninja=$(find target -name build.ninja -newer mcpp.toml | head -1)
            check_bytes "$ninja" "$name" "$label"
            check_bytes compile_commands.json "$name" "$label"
        fi
        ok "$label"
    done
done

# A path through a build program: MCPP_MANIFEST_DIR in, `mcpp:include-dir` out,
# and the header found at the directory the program printed.
dir="$ROOT/$CJK/buildprogram"
mkdir -p "$dir/src" "$dir/inc"
printf '[package]\nname    = "unicodebp"\nversion = "0.1.0"\n\n[toolchain]\nwindows = "llvm@20.1.7"\n' > "$dir/mcpp.toml"
printf '#define UNICODE_BP 42\n' > "$dir/inc/unicode_bp.h"
cat > "$dir/build.mcpp" <<'EOF'
#include <cstdio>
#include <cstdlib>
int main() {
    const char* here = std::getenv("MCPP_MANIFEST_DIR");
    if (!here) return 1;
    std::printf("mcpp:include-dir=%s/inc\n", here);
    return 0;
}
EOF
cat > "$dir/src/main.cpp" <<'EOF'
#include "unicode_bp.h"
import std;
int main() {
    std::println("build program {}", UNICODE_BP);
    return 0;
}
EOF
cd "$dir"
if "$MCPP" build > build.log 2>&1 && "$MCPP" run 2>&1 | grep -q 'build program 42'; then
    ok "a path through build.mcpp in '$CJK'"
else
    bad "a path through build.mcpp in '$CJK'"; tail -15 build.log | sed 's/^/          /'
fi

cd / && rm -rf "$ROOT"
[ "$failed" = 0 ] && echo "OK: every row builds in every directory" && exit 0
echo "FAIL: at least one row did not build in a directory whose name is not ASCII"
exit 1
