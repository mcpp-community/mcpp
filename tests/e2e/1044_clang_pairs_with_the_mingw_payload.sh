#!/usr/bin/env bash
# requires: mingw-cross wine
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1044 -- clang pairs with the MinGW-w64 payload (SPEC-016 §6).
#
# The `mingw` provider of `x86_64-windows-gnu` pairs with gcc and with llvm. With llvm declared,
# the payload its row names is installed as for gcc and is clang's sysroot: the C library,
# libstdc++ (whose `bits/std.cc` is the std module) and the GCC runtime; lld links.
#
# Criteria: the build reports the pairing; the compile line carries the payload as `--sysroot`
# and `-stdlib=libstdc++`; the program, which uses `import std` and `std::println`, runs under
# wine; its imports are the system's DLLs only. The llvm payload is installed by the build itself
# when this machine has none (the `--toolchain` request), so the test needs only the MinGW-w64 cross
# payload and wine.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'EOF'
[package]
name    = "paired"
version = "0.1.0"
EOF
cat > "$t/src/main.cpp" <<'EOF'
import std;
int main() {
    std::vector<std::string> words{"paired", "with", "mingw"};
    std::println("{}", words.size() == 3 ? "clang and libstdc++" : "wrong");
}
EOF
cd "$t"

out=$("$MCPP" build --target x86_64-windows-gnu --toolchain "llvm@${LLVM_VERSION}" 2>&1) || fail "the paired build failed" "$out"
grep -q "Pairs clang .* with the gcc@.* payload of x86_64-windows-gnu (mingw)" <<<"$out" || fail "the pairing is not reported" "$out"
ninja=$(find target/x86_64-windows-gnu -name build.ninja | head -1)
cxx=$(grep '^cxxflags' "$ninja")
grep -q -- "--sysroot=.*mingw-cross-gcc" <<<"$cxx" && grep -q -- "-stdlib=libstdc++" <<<"$cxx" \
    || fail "the compile line does not name the payload and libstdc++" "$cxx"
echo "  ok  clang compiles against the MinGW-w64 payload and its libstdc++"

exe=$(find target/x86_64-windows-gnu -path '*/bin/paired.exe' -type f | head -1)
[ -n "$exe" ] || fail "no paired.exe"
run=$(wine "$exe" 2>/dev/null | tr -d '\r') || fail "the program did not run under wine" "$run"
[ "$run" = "clang and libstdc++" ] || fail "the program printed '$run'"
dlls=$("$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-objdump" -p "$exe")
if grep 'DLL Name' <<<"$dlls" | grep -viqE 'kernel32|msvcrt|api-ms-win|ucrtbase'; then
    fail "the program needs a DLL of the payload" "$(grep 'DLL Name' <<<"$dlls")"
fi
echo "  ok  import std and std::println run under wine, with the system's DLLs only"
echo "PASS: 1044 clang pairs with the mingw payload"
