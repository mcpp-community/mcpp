#!/usr/bin/env bash
# requires: mingw-cross
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1058 -- a GNU-ABI Windows row compiles its resources with the tool its toolchain carries (design
# 2026-11-10 E3; mcpp#911 N9).
#
# clang paired with the MinGW-w64 payload looked for a windres beside clang only and refused
# `[resources]`, while the payload's `bin/` held one. The search now includes the paired payload;
# a clang with no windres anywhere compiles with the `llvm-rc` beside it, whose `.res` `ld.lld`
# links on this row. A script that includes the toolchain's `"winres.h"` reads it from the
# payload's headers, not from the project: the paired payload's under llvm, and under the cross
# gcc of the `mingw` provider, which is its own payload, `<root>/x86_64-w64-mingw32/include`.
#
# Criteria: the program builds with `[resources]` under llvm paired with MinGW-w64 and under the
# cross gcc, and carries a VERSIONINFO resource each time.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'TOML'
[package]
name    = "verinfo"
version = "0.1.0"

[resources]
files = ["app.rc"]

[targets.verinfo]
kind = "bin"
main = "src/main.c"
TOML
echo 'int main(void) { return 0; }' > "$t/src/main.c"
cat > "$t/app.rc" <<'RC'
#include "winres.h"
1 VERSIONINFO
FILEVERSION 1,2,3,4
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "ProductName", "verinfo"
    END
  END
END
RC
cd "$t"
out=$("$MCPP" build --target x86_64-windows-gnu --toolchain "llvm@${LLVM_VERSION}" 2>&1) \
    || fail "[resources] did not build on x86_64-windows-gnu with llvm" "$out"
exe=$(find target/x86_64-windows-gnu -path '*/bin/verinfo.exe' -type f | head -1)
[ -n "$exe" ] || fail "no program"
# The llvm payload of the build, not the host's tools.
readobj="$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-readobj"
[ -x "$readobj" ] || fail "no llvm-readobj in the llvm payload ($readobj)"
"$readobj" --coff-resources "$exe" | grep -q 'VERSIONINFO' || fail "the program carries no VERSIONINFO" "$("$readobj" --coff-resources "$exe" | head -20)"
# The cross gcc: before, its `"winres.h"` was taken for a file of the project that no action
# produces (`ninja: error: '<project>/winres.h' ... missing`).
rm -rf target
out=$("$MCPP" build --target x86_64-windows-gnu 2>&1) \
    || fail "[resources] did not build on x86_64-windows-gnu with the cross gcc" "$out"
exe=$(find target/x86_64-windows-gnu -path '*/bin/verinfo.exe' -type f | head -1)
[ -n "$exe" ] || fail "no program from the cross gcc"
"$readobj" --coff-resources "$exe" | grep -q 'VERSIONINFO' || fail "the cross gcc's program carries no VERSIONINFO"
echo "PASS: 1058 a GNU-ABI Windows row compiles its resources"
