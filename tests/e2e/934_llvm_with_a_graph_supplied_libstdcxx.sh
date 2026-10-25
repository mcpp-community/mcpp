#!/usr/bin/env bash
# 934 -- an llvm build whose C++ layer is libstdc++, supplied by a package of
# the graph (design 2026-10-26 §6.2, D10; the shape of `gcc.libstdcxx`).
#
# The package links the gcc payload's headers and runtime directory into its
# own tree and declares `provides = ["mcpp:c++-abi=libstdc++"]`, its std module
# (`bits/std.cc`) and its library directory. Criteria:
#   A  `import std` builds and runs; the typed catch works;
#   B  a C++ shared library of the build throws, the program catches
#      `const std::exception&` across the .so (one runtime: U, nothing to
#      verify as an island); the image needs no libc++;
#   C  a package that requires `mcpp:c++-abi=libstdc++` is satisfied by it;
#   D  without the provider the same requirement is refused when planned.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
source "$(dirname "$0")/_toolchain_env.sh"
[[ "$(uname -s)" == Linux ]] || { echo "SKIP: 934 uses the Linux gcc payload"; exit 0; }
[[ -x "$LLVM_ROOT/bin/clang++" ]] || { echo "SKIP: no llvm payload"; exit 0; }
G="$MCPP_E2E_GCC_ROOT"
[[ -e "$G/include/c++/$GCC_VERSION/bits/std.cc" && -e "$G/lib64/libstdc++.so.6" ]] \
    || { echo "SKIP: no gcc payload with bits/std.cc"; exit 0; }

# The provider, as `gcc.libstdcxx`'s install() lays it out.
P="$TMP/libstdcxx"
mkdir -p "$P/lib"
ln -s "$G/include/c++/$GCC_VERSION" "$P/cxx"
for f in libstdc++.so libstdc++.so.6 libstdc++.a libgcc_s.so libgcc_s.so.1; do
    [[ -e "$G/lib64/$f" ]] && ln -s "$G/lib64/$f" "$P/lib/$f"
done
cat > "$P/mcpp.toml" <<'EOF'
[package]
namespace = "gcc"
name      = "libstdcxx"
version   = "16.1.0.1"
provides  = ["mcpp:c++-abi=libstdc++"]
requires  = ["mcpp:compiler=llvm", "mcpp:c-abi=glibc"]
[build]
sources      = []
include_dirs = ["cxx", "cxx/x86_64-linux-gnu", "cxx/backward"]
std-module   = "cxx/bits/std.cc"
[runtime]
library_dirs = ["lib"]
EOF

# A requirer, as `compat.sycl-runtime` is.
R="$TMP/needs"
mkdir -p "$R/src"
cat > "$R/mcpp.toml" <<'EOF'
[package]
name     = "needs"
version  = "0.1.0"
requires = ["mcpp:c++-abi=libstdc++"]
[build]
sources = []
EOF

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name     = "app"
version  = "0.1.0"
standard = "c++23"
[toolchain]
default = "llvm@$LLVM_VERSION"
[lib]
path = "src/xrt.cppm"
[targets.xrt]
kind = "shared"
[targets.app]
kind = "bin"
main = "src/main.cpp"
[dependencies]
libstdcxx = { path = "../libstdcxx" }
needs     = { path = "../needs" }
EOF
printf 'export module xrt;\nimport std;\nexport namespace xrt { int checked(int v) { if (v < 0) throw std::runtime_error("negative"); return v * 2; } }\n' > "$TMP/app/src/xrt.cppm"
cat > "$TMP/app/src/main.cpp" <<'EOF'
import std;
import xrt;
int main() {
    std::vector<int> v{3, 1, 2};
    std::ranges::sort(v);
    try { xrt::checked(-1); }
    catch (const std::exception& e) { std::println("{} caught {}", v, e.what()); return 0; }
    catch (...) { std::println("missed"); return 1; }
}
EOF
( cd "$TMP/app" && "$MCPP" run app > out.log 2>&1 ) || fail "A/B/C: the build or the run failed" "$TMP/app/out.log"
grep -q "caught negative" "$TMP/app/out.log" || fail "B: no typed catch across the shared library" "$TMP/app/out.log"
grep -q "c++-abi.*libstdc++" "$TMP/app/out.log" || true
APP=$(find "$TMP/app/target" -path '*/bin/app' -type f | head -1)
readelf -d "$APP" | grep -q 'libc++' && fail "B: the image needs libc++" "$TMP/app/out.log"
SO=$(find "$TMP/app/target" -name 'libxrt.so*' -type f | head -1)
[[ -n "$SO" ]] || fail "B: no libxrt.so" "$TMP/app/out.log"
readelf -d "$SO" | grep -q 'libc++' && fail "B: the shared library needs libc++" "$TMP/app/out.log"

# D: no provider, the requirement is refused before anything compiles.
sed -i '/^libstdcxx = /d' "$TMP/app/mcpp.toml"
set +e
( cd "$TMP/app" && "$MCPP" build > d.log 2>&1 ); rc=$?
set -e
[[ $rc -ne 0 ]] || fail "D: the requirement was not refused" "$TMP/app/d.log"
grep -q "requires the c++-abi to be .libstdc++." "$TMP/app/d.log" \
    || fail "D: the refusal does not name the requirement" "$TMP/app/d.log"

echo "PASS: 934 llvm with a graph-supplied libstdc++"
