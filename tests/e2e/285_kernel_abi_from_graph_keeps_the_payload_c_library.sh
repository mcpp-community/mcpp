#!/usr/bin/env bash
# requires: gcc unix-shell
# A package may supply the kernel interface while the C library stays the
# payload's, and the link line has to keep reaching the payload.
#
# ⚠️ THE PREDICATE THAT DECIDED THIS WAS AN `OR` OVER TWO LAYERS.
#
#     bool system_from_graph() const {
#         return kernelAbi.fromGraph() || cAbi.fromGraph();
#     }
#
# The link side replaced `f.ld` when that was true — dropping the payload's
# binutils prefix, its library directories and its rpaths, all of which are
# ways of reaching the payload's C LIBRARY. In the arrangement it was written
# for the two layers move together: an openkal target takes its kernel
# interface and its C library from the same graph. They come apart here, and
# the driver went on asking for startup files nobody had given it a path to:
#
#     error: hermetic link check failed
#              crt1.o (bare name — the linker cannot resolve it)
#              crti.o (bare name — the linker cannot resolve it)
#              crtn.o (bare name — the linker cannot resolve it)
#
# ⭐ THE SHAPE IS NOT EXOTIC — IT IS HOW EVERY openkal BACKEND IS TESTED.
# openkal-linux, openkal-macos and openkal-windows each build their conformance
# suite against the platform's own C library, because a backend implements
# openkal ON TOP OF that platform. All three went red the moment their CI pin
# moved onto the release carrying this, and stayed green before it — which is
# the only reason the defect shipped.
#
# ⚠️ AND NOTHING IN THIS SUITE HAD THIS SHAPE. 278 e2e scripts, and the
# combination "kernel-abi from the graph, C library from the payload" appeared
# in none of them. That is what this file is for.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app/src"
cd "$work/app"

# openkal-linux provides `mcpp:kernel-abi=openkal` and nothing below it. A
# program naming it alone gets its kernel interface from the graph and
# everything else — C library, C++ runtime, startup files — from the payload.
#
# ⚠️ AND WITHOUT `features = ["standalone"]`, WHICH IS A DIFFERENT SHAPE.
# That feature says this implementation is the whole of the program's
# environment, so it supplies the entry point; asked for beside a C library
# that supplies one too, the link ends in
#
#     multiple definition of `_start`
#
# which is both correct and not what this file is about. The conformance
# suites that hit the defect do not select it either — a backend is tested as
# a library on top of its platform, not as a replacement for it.
cat > mcpp.toml <<'TOML'
[package]
name    = "kabi"
version = "0.1.0"

[toolchain]
default = "gcc@16.1.0"

[dependencies]
openkal-linux = "0.5.4"
TOML
# ⭐ THE PROGRAM USES THE C++ RUNTIME, NOT ONLY THE C LIBRARY.
#
# `int main() { return 0; }` links against almost nothing and would pass while
# a second defect of the same family was live: the contract table decided
# whether the payload's C++ archives serve this target with the same two-layer
# OR, so `-nostdlib++` was emitted for this shape and a program that throws
# could not link —
#
#     undefined reference to `__cxa_allocate_exception'
#     undefined reference to `std::runtime_error::runtime_error(char const*)'
#
# A `throw` and a `std::string` reach the C++ runtime and the C library
# respectively, so one program covers both layers this shape gets wrong.
cat > src/main.cpp <<'CPP'
#include <string>
#include <stdexcept>
#include <cstdio>

int main() {
    try { throw std::runtime_error("boom"); }
    catch (const std::exception& e) { std::printf("%s\n", e.what()); }
    std::string s = "x"; s += "y";
    return s == "xy" ? 0 : 1;
}
CPP

out="$("$MCPP" build 2>&1)" && rc=0 || rc=$?

# The target side must report what it actually is: the kernel interface from
# the graph, and no `c-abi … graph` line beside it. If that ever stops being
# true this test is measuring something else and should be re-read, not
# re-pinned.
case "$out" in
  *"kernel-abi"*"graph"*) ;;
  *)
    echo "SKIP: the graph did not supply the kernel interface here"
    printf '%s\n' "$out" | grep -iE 'error|abi' | head -3
    exit 0 ;;
esac
case "$out" in
  *"c-abi"*"graph"*)
    echo "SKIP: the C library came from the graph too — not the shape under test"
    exit 0 ;;
esac

if [ "$rc" != 0 ]; then
    echo "FAIL: the build failed with the C library on the payload"
    printf '%s\n' "$out" | grep -iE 'error|crt|bare name|outside the sandbox' | head -6
    exit 1
fi

# ⭐ AND THE ARTEFACT, BECAUSE A LINK THAT SUCCEEDS IS NOT THE CLAIM.
# The claim is that the payload's C runtime was reached; a program that links
# without a C library at all would also exit 0 here.
bin="$(find target -type f -name kabi | head -1)"
[ -n "$bin" ] || { echo "FAIL: no artefact was produced"; exit 1; }

if out="$("$bin" 2>&1)"; then
    [ "$out" = "boom" ] || { echo "FAIL: wrong output: $out"; exit 1; }
    echo "  ok  it links against the payload's C library and C++ runtime, and runs"
else
    echo "FAIL: the artefact does not run"
    exit 1
fi

echo "OK: a graph-supplied kernel interface leaves the payload's C library reachable"
