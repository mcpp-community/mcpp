#!/usr/bin/env bash
# requires: elf
# 657 -- a project with a build.mcpp builds for wasm32-emscripten (#622).
#
# `build.mcpp` is compiled and run on this machine. Under a cross target the
# engine resolves a host toolchain for it from the toolchain spec, and the
# spec was read AFTER the target row's convention had replaced it: for the
# Web row that is `emsdk@6.0.9`, whose `em++` produces WebAssembly under every
# invocation. Every project with a build program therefore failed under
# `--target wasm32-emscripten`, inside emcc.py (`AssertionError ...
# phase_compile_inputs`), measured 2026-09-12 by the first dist-web build. An
# NDK clang can also target the host, which is why the Android rows never
# showed it.
#
# The engine now keeps the spec as it stood before the row's pin replaced it
# and resolves the build program's compiler from that. Asserted here as the
# only criterion that distinguishes the two: the build succeeds, the program
# runs under node, and the build program's `MCPP_HOST` equals the machine's
# triple while `MCPP_TARGET` is the Web row. Skips honestly where no emsdk
# payload is installed, as 650 does.
set -e

have_emsdk=0
for d in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-emsdk/*/emscripten \
         "$HOME"/.xlings/data/xpkgs/xim-x-emsdk/*/emscripten; do
    [[ -x "$d/em++" ]] && have_emsdk=1
done
if [ "$have_emsdk" -ne 1 ]; then
    echo "SKIP: 657 -- no emsdk payload installed (looked under xim-x-emsdk/*/emscripten)"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/p/src"
cat > "$TMP/p/mcpp.toml" <<'TOML'
[package]
name = "bp"
version = "0.1.0"

[targets.bp]
kind = "bin"
main = "src/main.cpp"

[target.wasm32-emscripten]
runner = ["node"]
TOML
cat > "$TMP/p/src/main.cpp" <<'CPP'
#include <cstdio>
int main() { std::puts("1-2-3"); return 0; }
CPP
# The build program records the two triples it was told, so the test can say
# which machine it was compiled for without reading a compiler's argv.
cat > "$TMP/p/build.mcpp" <<'CPP'
import mcpp;
#include <cstdio>
int main() {
    std::FILE* f = std::fopen("bp-env.txt", "w");
    if (!f) return 1;
    std::fprintf(f, "host=%s\ntarget=%s\n", mcpp::host(), mcpp::target());
    std::fclose(f);
    return 0;
}
CPP

cd "$TMP/p"
"$MCPP" build --target wasm32-emscripten > build.log 2>&1 \
    || fail "a project with a build.mcpp does not build for wasm32-emscripten (the host compiler for build.mcpp was the row's em++)" build.log
grep -q 'AssertionError' build.log && fail "emcc.py's assertion is in the log" build.log
[ -f bp-env.txt ] || fail "the build program did not run" build.log
host_triple=$(grep '^host=' bp-env.txt | cut -d= -f2)
target_triple=$(grep '^target=' bp-env.txt | cut -d= -f2)
[ "$target_triple" = "wasm32-emscripten" ] || fail "MCPP_TARGET was '$target_triple'" bp-env.txt
case "$host_triple" in wasm32*|"") fail "MCPP_HOST was '$host_triple'" bp-env.txt ;; esac
[ -n "$(find target/wasm32-emscripten -path '*/bin/bp.js' | head -1)" ] || fail "no bin/bp.js" build.log
"$MCPP" run --target wasm32-emscripten > run.log 2>&1 || fail "mcpp run failed" run.log
grep -qx '1-2-3' run.log || fail "the program did not print 1-2-3" run.log

echo "PASS: 657_a_build_program_under_the_web_row_uses_the_host_compiler"
