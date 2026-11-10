#!/usr/bin/env bash
# requires: llvm wine
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1063 -- a MinGW-w64 base package serves the GNU-ABI Windows rows beside llvm (design 2026-11-10
# E9; mcpp#913; SPEC-016 §6).
#
# The `mingw-llvm` provider names `xim:mingw-w64-sysroot`, the target side of llvm-mingw (MinGW-w64
# on UCRT, libc++, libunwind, compiler-rt) as a base package. It is the driver's sysroot; the base's
# libc++ module sources are the std module; its compiler runtime is read from the directory the base
# names. No gcc payload takes part, so every host reaches both architectures. The provider is chosen
# by name here because this host has the `mingw` payload, which outranks it.
#
# Criteria: the build reports the base's C library (ucrt), c++-abi and compiler-runtime; the compile line carries
# `-stdlib=libc++` and the base as `--sysroot`; the program, which uses `import std`, runs under
# wine and imports system DLLs only (the default link is static); the same project builds an Aarch64
# PE for `aarch64-windows-gnu`, a row no gcc payload serves.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'EOF2'
[package]
name    = "based"
version = "0.1.0"

[target.x86_64-windows-gnu]
platform = "builtin:mingw-llvm"

[target.aarch64-windows-gnu]
platform = "builtin:mingw-llvm"
EOF2
cat > "$t/src/main.cpp" <<'EOF2'
import std;
int main() {
    std::vector<std::string> words{"a", "mingw", "base"};
    std::println("{}", words.size() == 3 ? "clang and libc++" : "wrong");
}
EOF2
cd "$t"

out=$("$MCPP" build --target x86_64-windows-gnu 2>&1) || fail "the build over the base failed" "$out"
grep -qE 'c-abi +ucrt +\(' <<<"$out" || fail "the base's C library is not reported by its name" "$out"
grep -qE 'c\+\+-abi +libc\+\+ +\(mingw-w64-sysroot@' <<<"$out" || fail "the base's C++ layer is not reported" "$out"
grep -qE 'compiler-runtime +compiler-rt +\(mingw-w64-sysroot@' <<<"$out" || fail "the base's compiler runtime is not reported" "$out"
ninja=$(find target/x86_64-windows-gnu -name build.ninja | head -1)
cxx=$(grep '^cxxflags' "$ninja")
grep -q -- "--sysroot=.*mingw-w64-sysroot" <<<"$cxx" && grep -q -- "-stdlib=libc++" <<<"$cxx" \
    || fail "the compile line does not name the base and libc++" "$cxx"
echo "  ok  clang compiles against the base and its libc++"

exe=$(find target/x86_64-windows-gnu -path '*/bin/based.exe' -type f | head -1)
[ -n "$exe" ] || fail "no based.exe"
run=$(wine "$exe" 2>/dev/null | tr -d '\r') || fail "the program did not run under wine" "$run"
[ "$run" = "clang and libc++" ] || fail "the program printed '$run'"
objdump="$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-objdump"
if "$objdump" -p "$exe" | grep 'DLL Name' | grep -viqE 'kernel32|api-ms-win|ucrtbase'; then
    fail "the program needs a DLL of the base" "$("$objdump" -p "$exe" | grep 'DLL Name')"
fi
echo "  ok  import std runs under wine, with the system's DLLs only"

out=$("$MCPP" build --target aarch64-windows-gnu 2>&1) || fail "the aarch64 build failed" "$out"
exe=$(find target/aarch64-windows-gnu -path '*/bin/based.exe' -type f | head -1)
"$objdump" -f "$exe" | grep -qi 'aarch64\|arm64' || fail "not an Aarch64 PE" "$("$objdump" -f "$exe")"
echo "  ok  aarch64-windows-gnu, which no gcc payload serves, builds"
echo "PASS: 1063 a mingw base package serves windows-gnu beside llvm"
