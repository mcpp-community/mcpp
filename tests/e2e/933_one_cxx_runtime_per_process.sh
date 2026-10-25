#!/usr/bin/env bash
# 933 -- one C++ runtime per process, and the islands that may differ
# (design 2026-10-26 §6.3, SPEC-002 §6; mcpp#838).
#
# An llvm build (libc++) with libstdc++ named on its link line:
#   A  a static library and a test that catches `std::exception` thrown by it:
#      the line puts libstdc++ AFTER the build's own runtime, so the typed catch
#      works (the previous release split the ABI layer and only `catch (...)`
#      caught it), and the undeclared island is reported;
#   B  the same with a C++ shared library target: refused when planned,
#      [cxx-runtime-island];
#   C  a library built by g++ (on libstdc++) whose exception type the llvm
#      program catches by type: refused after the link, [cxx-runtime-collision];
#   D  the island declared, `requires = [{ layer = "mcpp:c++-abi=libstdc++",
#      visibility = "private" }]` under `[target.'cfg(os = "linux")']`: no
#      report;
#   E  `visibility = "private"` on another layer: a manifest error naming
#      requires-private-layer.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
source "$(dirname "$0")/_toolchain_env.sh"
[[ "$(uname -s)" == Linux ]] || { echo "SKIP: 933 is about ELF"; exit 0; }
[[ -x "$LLVM_ROOT/bin/clang++" ]] || { echo "SKIP: no llvm payload"; exit 0; }
GLIB="$MCPP_E2E_GCC_ROOT/lib64"
[[ -e "$GLIB/libstdc++.so.6" && -x "$MCPP_E2E_GCC_ROOT/bin/g++" ]] || { echo "SKIP: no gcc payload"; exit 0; }

project() {   # $1 dir, $2 target kind, $3 extra manifest text
    mkdir -p "$1/src" "$1/tests"
    cat > "$1/mcpp.toml" <<EOF
[package]
name     = "xrt"
version  = "0.1.0"
standard = "c++23"
[toolchain]
default = "llvm@$LLVM_VERSION"
[lib]
path = "src/xrt.cppm"
[targets.xrt]
kind = "$2"
[build]
ldflags = ["-L$GLIB", "-l:libstdc++.so.6"]
$3
EOF
    printf 'export module xrt;\nimport std;\nexport namespace xrt { int checked(int v) { if (v < 0) throw std::runtime_error("negative"); return v * 2; } }\n' > "$1/src/xrt.cppm"
    cat > "$1/tests/catch.cpp" <<'EOF'
import std;
import xrt;
int main() {
    auto ep = std::make_exception_ptr(std::logic_error("x"));   // a libc++abi-only entry point
    try { xrt::checked(-1); }
    catch (const std::exception& e) { std::println("caught as std::exception: {}", e.what()); return 0; }
    catch (...)                     { std::println("std::exception missed -> (...)"); return 1; }
}
EOF
}

# A
project "$TMP/a" lib ""
( cd "$TMP/a" && "$MCPP" test > out.log 2>&1 ) || fail "A: the typed catch failed" "$TMP/a/out.log"
grep -q "caught as std::exception" "$TMP/a/out.log" || fail "A: no typed catch" "$TMP/a/out.log"
grep -q "no package declares it" "$TMP/a/out.log" || fail "A: the undeclared island is not reported" "$TMP/a/out.log"

# B
project "$TMP/b" shared ""
set +e
( cd "$TMP/b" && "$MCPP" build > out.log 2>&1 ); rc=$?
set -e
[[ $rc -ne 0 ]] || fail "B: a C++ shared library beside the island was accepted" "$TMP/b/out.log"
grep -q "cxx-runtime-island" "$TMP/b/out.log" || fail "B: not refused as an island" "$TMP/b/out.log"

# C -- libfoo is built by mcpp with the gcc payload, so it is a C++ library
# on libstdc++ whose exception type crosses its interface.
mkdir -p "$TMP/c/foo/src" "$TMP/c/app/src"
cat > "$TMP/c/foo/mcpp.toml" <<EOF
[package]
name     = "foo"
version  = "0.1.0"
standard = "c++23"
[toolchain]
default = "gcc@$GCC_VERSION"
[build]
sources = ["src/*.cpp"]
[targets.foo]
kind = "shared"
EOF
cat > "$TMP/c/foo/src/foo.cpp" <<'EOF'
#include <stdexcept>
struct FooError : std::runtime_error { FooError() : std::runtime_error("foo") {} };
void foo_throws() { throw FooError(); }
EOF
( cd "$TMP/c/foo" && "$MCPP" build > out.log 2>&1 ) || fail "C: libfoo did not build with gcc" "$TMP/c/foo/out.log"
FOO_DIR=$(dirname "$(find "$TMP/c/foo/target" -name 'libfoo.so' | head -1)")
[[ -e "$FOO_DIR/libfoo.so" ]] || fail "C: no libfoo.so" "$TMP/c/foo/out.log"
cat > "$TMP/c/app/mcpp.toml" <<EOF
[package]
name     = "app"
version  = "0.1.0"
standard = "c++23"
[toolchain]
default = "llvm@$LLVM_VERSION"
[build]
ldflags = ["-L$FOO_DIR", "-lfoo", "-Wl,-rpath,$FOO_DIR"]
EOF
cat > "$TMP/c/app/src/main.cpp" <<'EOF'
#include <stdexcept>
struct FooError : std::runtime_error { FooError(); };
void foo_throws();
int main() { try { foo_throws(); } catch (const FooError&) { return 0; } return 1; }
EOF
set +e
( cd "$TMP/c/app" && "$MCPP" build > out.log 2>&1 ); rc=$?
set -e
[[ $rc -ne 0 ]] || fail "C: a typed catch across runtimes was accepted" "$TMP/c/app/out.log"
grep -q "cxx-runtime-collision" "$TMP/c/app/out.log" || fail "C: not refused as a collision" "$TMP/c/app/out.log"

# D
project "$TMP/d" lib "[target.'cfg(os = \"linux\")']
requires = [{ layer = \"mcpp:c++-abi=libstdc++\", visibility = \"private\" }]"
( cd "$TMP/d" && "$MCPP" test > out.log 2>&1 ) || fail "D: the declared island failed" "$TMP/d/out.log"
! grep -q "no package declares it" "$TMP/d/out.log" || fail "D: a declared island is reported as undeclared" "$TMP/d/out.log"

# E
mkdir -p "$TMP/e/src"
printf '[package]\nname = "e"\nversion = "0.1.0"\nrequires = [{ layer = "mcpp:c-abi=glibc", visibility = "private" }]\n' > "$TMP/e/mcpp.toml"
printf 'int main() { return 0; }\n' > "$TMP/e/src/main.cpp"
set +e
( cd "$TMP/e" && "$MCPP" build > out.log 2>&1 ); rc=$?
set -e
[[ $rc -ne 0 ]] && grep -q "requires-private-layer" "$TMP/e/out.log" \
    || fail "E: private on another layer was accepted" "$TMP/e/out.log"

echo "PASS: 933 one C++ runtime per process, and verified islands"
