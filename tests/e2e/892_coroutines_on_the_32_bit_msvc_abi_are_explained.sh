#!/usr/bin/env bash
# requires: msvc
# 892 -- clang that does not support coroutines on the 32-bit x86 Microsoft
# ABI: mcpp follows the compiler and explains the failure (2026.10.8.1,
# LLVM 23.1.3 Part 3, D1).
#
# clang 23 does not predefine `__cpp_impl_coroutine` for i686-pc-windows-msvc,
# so the MSVC STL's <coroutine> is empty. Two failures follow, and neither
# names its cause: the C++23 std module stops inside <generator>, and code
# that uses coroutines stops at `use of undeclared identifier 'std'`. mcpp
# neither defines the macro nor rewrites the std module; it appends a note.
#
#   A  C++23 `import std`: fails; the note offers C++20 first and llvm 22.1.8
#      as an option;
#   B  C++20 `import std` and `import std.compat`: build and run, no note;
#   C  C++20 code that uses coroutines: fails; the note offers no C++20.
#
# The expectation follows the compiler: when it predefines the macro for this
# target (an LLVM before 23, or a later one that enables coroutines again),
# A and C build and no note may appear.
set -e
source "$(dirname "$0")/_host_path.sh"
source "$(dirname "$0")/_toolchain_env.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
REGISTRY="${MCPP_HOME:-$HOME/.mcpp}/registry"
REGISTRY_HOST=$(host_path "$REGISTRY")
export MCPP_HOME="$TMP/mcpp-home"
mkdir -p "$MCPP_HOME"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF

NOTE="does not support C++20 coroutines"
project() {   # project <dir> <standard> <source>
    mkdir -p "$TMP/$1/src"
    cat > "$TMP/$1/mcpp.toml" <<EOF
[package]
name = "$1"
version = "0.1.0"
standard = "$2"

[target.i686-windows-msvc]
toolchain = "llvm@$LLVM_VERSION"
EOF
    printf '%s\n' "$3" > "$TMP/$1/src/main.cpp"
}

project cxx23 c++23 'import std;
int main() { std::println("x86 std {}", sizeof(void*)); }'
project cxx20 c++20 'import std;
int main() { std::cout << "x86 std " << sizeof(void*) << "\n"; }'
project compat c++20 'import std.compat;
int main() { std::printf("x86 compat %zu\n", sizeof(void*)); }'
project coro c++20 '#include <coroutine>
struct task { struct promise_type {
    task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {} }; };
task f() { co_return; }
int main() { f(); }'

# B first: it installs the toolchain the expectation is read from.
cd "$TMP/cxx20"
"$MCPP" run --target i686-windows-msvc > b.log 2>&1 || fail "B: C++20 import std failed" b.log
grep -q "x86 std 4" b.log || fail "B: the 32-bit program did not run" b.log
grep -q "$NOTE" b.log && fail "B: a successful build carries the note" b.log
cd "$TMP/compat"
"$MCPP" run --target i686-windows-msvc > b2.log 2>&1 || fail "B: C++20 import std.compat failed" b2.log
grep -q "x86 compat 4" b2.log || fail "B: the std.compat program did not run" b2.log

clang="$REGISTRY/data/xpkgs/xim-x-llvm/$LLVM_VERSION/bin/clang++.exe"
[ -x "$clang" ] || clang="$REGISTRY/data/xpkgs/xim-x-llvm/$LLVM_VERSION/bin/clang++"
: > "$TMP/empty.cpp"
if "$clang" --no-default-config --target=i686-pc-windows-msvc -std=c++23 -dM -E -x c++ \
        "$(host_path "$TMP/empty.cpp")" | grep -q "__cpp_impl_coroutine"; then
    supported=1
else
    supported=0
fi

cd "$TMP/cxx23"
if [ "$supported" = 1 ]; then
    "$MCPP" run --target i686-windows-msvc > a.log 2>&1 || fail "A: llvm@$LLVM_VERSION supports coroutines here, yet C++23 import std failed" a.log
    grep -q "$NOTE" a.log && fail "A: the note appeared for a compiler that supports coroutines" a.log
    echo "PASS: 892 llvm@$LLVM_VERSION supports coroutines on i686-windows-msvc; no note"
    exit 0
fi
if "$MCPP" build --target i686-windows-msvc > a.log 2>&1; then fail "A: C++23 import std built" a.log; fi
grep -q "$NOTE" a.log || fail "A: the failure carries no note" a.log
grep -q "<generator>" a.log || fail "A: the note does not name <generator>" a.log
c20=$(grep -n 'standard = "c++20"' a.log | head -1 | cut -d: -f1)
l22=$(grep -n 'toolchain = "llvm@22.1.8"' a.log | head -1 | cut -d: -f1)
[ -n "$c20" ] && [ -n "$l22" ] && [ "$c20" -lt "$l22" ] \
    || fail "A: the note does not offer C++20 first and llvm 22.1.8 second" a.log

cd "$TMP/coro"
if "$MCPP" build --target i686-windows-msvc > c.log 2>&1; then fail "C: coroutine code built" c.log; fi
grep -q "$NOTE" c.log || fail "C: the failure carries no note" c.log
grep -q 'standard = "c++20"' c.log && fail "C: C++20 was offered for code that uses coroutines" c.log
grep -q 'toolchain = "llvm@22.1.8"' c.log || fail "C: llvm 22.1.8 was not offered" c.log

echo "PASS: 892 coroutines on the 32-bit MSVC ABI are explained"
