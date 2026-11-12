#!/usr/bin/env bash
# requires: llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1075 -- a Windows row whose system the graph supplies compiles its resources
# with the Windows headers the platform's rows declare (2026.11.11.1, G3;
# design 2026-11-10 E3's third source).
#
# `x86_64-windows-musl` takes its system from the graph (openkal-platform's
# hosted part), so its toolchain has no paired payload, and its llvm binary
# carries no Windows headers of its own: neither include source
# `system_include_dirs` read existed, and a script that includes
# `"winres.h"` failed in llvm-rc with `winres.h: No such file or directory`.
# The MinGW-w64 sysroot payload a platform row declares for the
# architecture's GNU-ABI identity holds them, under one `include/` for every
# architecture; every PE row shares that set, and the two existing sources
# keep their precedence.
#
# Criteria: the project builds with `[resources]` on x86_64-windows-musl and
# the program carries a VERSIONINFO resource; a GNU-ABI row (1058) is not
# re-tested here.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -25; exit 1; }
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
out=$("$MCPP" build --target x86_64-windows-musl --toolchain "llvm@${LLVM_VERSION}" 2>&1) \
    || fail "[resources] did not build on x86_64-windows-musl" "$out"
exe=$(find target/x86_64-windows-musl -path '*/bin/verinfo.exe' -type f | head -1)
[ -n "$exe" ] || fail "no program" "$out"
# The llvm payload of the build, not the host's tools.
readobj="$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-readobj"
[ -x "$readobj" ] || fail "no llvm-readobj in the llvm payload ($readobj)"
"$readobj" --coff-resources "$exe" | grep -q 'VERSIONINFO' \
    || fail "the program carries no VERSIONINFO" "$("$readobj" --coff-resources "$exe" | head -20)"
echo "PASS: 1075 a graph-supplied Windows row finds its resource headers"
