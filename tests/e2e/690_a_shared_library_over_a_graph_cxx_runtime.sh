#!/usr/bin/env bash
# requires: llvm elf
# 690 -- a dependency's C++ shared library in a graph whose C++ runtime is a
# package (#641, item 5). The runtime package's objects are linked into the
# program; the dependency's shared library was linked from its own objects with
# `-nostdlib++`, so it had no C++ runtime. The package compiles its runtime with
# hidden visibility, so the program's copy cannot serve it: on Linux `ld.lld`
# refused the program ("non-exported symbol ... referenced by DSO"), on macOS
# the dylib itself failed to link.
#
# The engine now refuses before compiling, naming the ways out, and links a
# private copy of the runtime into the shared library when the manifest states
# `cxx_runtime = { shared = "self-contained" }`. Read in four legs: the refusal
# for a package that constrains its own form, which names its statement and
# offers no edge remedy, with the refusal's token; the refusal for a form the
# consumer requested, which offers both remedies; the static edge, which builds
# and runs; the stated private copy, which builds, leaves the library with no
# undefined libc++ reference, and runs. The last leg also prints, as a reading, whether a standard
# exception thrown in the library is caught by its class in the program: with
# one private copy per image it is not, and that is the consequence the
# refusal states.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# The container job that runs this has no binutils; the llvm payload has nm.
NM=$(command -v nm || true)
[ -n "$NM" ] || NM=$(ls "$MCPP_HOME"/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/llvm-nm 2>/dev/null | head -1)

cd "$TMP"
mkdir -p fw/src app/src

write_fw() {    # $1 = the kind fw declares
    cat > fw/mcpp.toml <<TOML
[package]
name    = "fw"
version = "0.1.0"

[targets.fw]
kind = "$1"
TOML
}
cat > fw/src/fw.cppm <<'CPP'
export module fw;
import std;
export std::string fw_greet(int n);
export void fw_throw_runtime();
CPP
cat > fw/src/fw.cpp <<'CPP'
module fw;
import std;
std::string fw_greet(int n) { return std::format("fw-{}", n); }
void fw_throw_runtime() { throw std::runtime_error("from fw"); }
CPP

write_app() {   # $1 = the fw edge, $2 = extra [build] lines
    cat > app/mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[build]
$2

[dependencies]
llvm.libcxx = { git = "https://github.com/mcpplibs/libcxx.git", tag = "22.1.8.2" }
fw = $1
TOML
}
cat > app/src/main.cpp <<'CPP'
import std;
import fw;
int main() {
    std::cout << fw_greet(3) << std::endl;
    try { fw_throw_runtime(); }
    catch (const std::runtime_error&) { std::cout << "runtime_error caught by its class" << std::endl; }
    catch (...) { std::cout << "runtime_error not matched by its class" << std::endl; }
}
CPP

# ── The refusal, before anything compiles: the package constrains the form ─
write_fw shared
write_app '{ path = "../fw" }' ''
cd app
if "$MCPP" build > refused.log 2>&1; then
    fail "a shared library over a graph C++ runtime was not refused" refused.log
fi
grep -q "'fw' is linked as a shared library" refused.log \
    || fail "the refusal does not name the shared library" refused.log
grep -q "llvm.libcxx@22.1.8.2" refused.log || fail "the refusal does not name the provider" refused.log
grep -q 'cxx_runtime = { shared = "self-contained" }' refused.log \
    || fail "the refusal does not name the private-copy statement" refused.log
grep -q "not caught by that class" refused.log || fail "the refusal does not state the consequence" refused.log
grep -q "'fw' states its form itself (\[targets.fw\] kind = \"shared\")" refused.log \
    || fail "the refusal does not name the package's own statement" refused.log
grep -q 'linkage = "static" }' refused.log \
    && fail "the refusal offers an edge remedy the package's constraint defeats" refused.log
ninja=$(ls target/*/*/build.ninja 2>/dev/null | head -1 || true)
[ -z "$ninja" ] || ! grep -q '^build bin/libfw.so' "$ninja" \
    || fail "a build graph with the shared library was written before the refusal" "$ninja"
if command -v jq >/dev/null 2>&1; then
    "$MCPP" why toolchain --format json > why.json 2>/dev/null || true
    reason=$(jq -r '.data.reason // empty' why.json 2>/dev/null || true)
    [ "$reason" = "shared-library-cxx-runtime" ] \
        || fail "the machine reason was '$reason', expected shared-library-cxx-runtime" why.json
fi
echo "ok: a package-constrained shared library is refused before compiling"
cd ..

# ── The refusal when the consumer asked for the shared form: both remedies ──
write_fw lib
write_app '{ path = "../fw", linkage = "shared" }' ''
cd app
rm -rf target
if "$MCPP" build > requested.log 2>&1; then
    fail "a requested shared library over a graph C++ runtime was not refused" requested.log
fi
grep -q 'linkage = "static" }' requested.log \
    || fail "the refusal does not offer the static edge for a requested form" requested.log
grep -q 'cxx_runtime = { shared = "self-contained" }' requested.log \
    || fail "the refusal does not offer the private copy for a requested form" requested.log
echo "ok: a requested shared library is refused with both remedies"
cd ..

# ── The static edge: builds and runs, no shared library ──────────────────────
write_app '{ path = "../fw", linkage = "static" }' ''
cd app
rm -rf target
"$MCPP" build > static.log 2>&1 || fail "the static edge did not build" static.log
bin=$(ls target/*/*/bin/app | head -1)
ls target/*/*/bin/libfw.so > /dev/null 2>&1 && fail "the static edge still produced libfw.so" static.log
out=$("$bin") || fail "the program over the static edge exited non-zero"
echo "$out" | grep -qx "fw-3" || fail "expected fw-3 from the static edge, got: $out"
echo "ok: linkage = \"static\" builds and runs"
cd ..

# ── The stated private copy: builds, no undefined libc++ reference, runs ────
write_fw shared
write_app '{ path = "../fw" }' 'cxx_runtime = { shared = "self-contained" }'
cd app
rm -rf target
"$MCPP" build > private.log 2>&1 || fail "the stated private copy did not build" private.log
so=$(ls target/*/*/bin/libfw.so | head -1)
[ -n "$so" ] || fail "no libfw.so under the stated private copy" private.log
[ -n "$NM" ] || fail "no nm and no llvm-nm to read libfw.so with"
undefined=$("$NM" -D --undefined-only "$so" | grep -c '__1' || true)
[ "$undefined" -eq 0 ] || fail "libfw.so still has $undefined undefined std::__1 references" private.log
bin=$(ls target/*/*/bin/app | head -1)
out=$("$bin") || fail "the program over the private copy exited non-zero"
echo "$out" | grep -qx "fw-3" || fail "expected fw-3 under the private copy, got: $out"
echo "READING private copy: $(echo "$out" | grep runtime_error)"
echo "ok: cxx_runtime = { shared = \"self-contained\" } links a private copy and runs"

echo "PASS: 690 a shared library over a graph C++ runtime is refused or carries a stated private copy"
