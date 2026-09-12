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
#
# PHASE 2, below, closes the gap the first fix left open: "the spec as it
# stood before the row's pin" only exists when SOMETHING was there to stand —
# a [toolchain], a global default, a [target.<row>] entry. On a fresh
# $MCPP_HOME whose very first invocation names `--target wasm32-emscripten`
# (no prior default has ever been resolved or persisted), there is nothing to
# restore, and the engine fell through to the row's OWN pin again — the exact
# defect this file exists to catch, just with no earlier value standing in
# the way. Measured against the released 2026.9.12.3 binary: the build
# resolved "host toolchain for build.mcpp: clang 6.0.9
# (wasm32-unknown-emscripten)" and then failed inside emcc.py's
# `phase_compile_inputs` with the same `AssertionError` as the original
# report — reusing a project that already imports `mcpp` reproduces it
# without needing `mcpp:plugins`. The fix resolves the platform's own native
# default in that case (the same one a plain `mcpp build` would install),
# not the row's convention. Phase 2 holds a fresh $MCPP_HOME so this is the
# condition actually exercised, and phase 3 checks the fix did not cost the
# native path anything in that same fresh home.
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

# ── PHASE 1: the ambient $MCPP_HOME (a global default toolchain typically
# already exists here — "the spec as it stood before the row's pin") ───────
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

# ── PHASE 2: a FRESH $MCPP_HOME whose first-ever invocation is the cross
# build — no [toolchain], no global default, nothing for `hostSpecBeforeRowPin`
# to carry forward. This is the condition the original fix's own comment
# named and left open ("Empty when the row replaced nothing, in which case
# the row's pin remains the only spec there is") and is exactly what a fresh
# sandbox $HOME holds on its first `mcpp build --target wasm32-emscripten` —
# see .agents/docs/2026-09-12-622-verify.sh section J, measured against the
# released 2026.9.12.3 binary.
FRESH_HOME="$TMP/fresh-mcpp-home"
mkdir -p "$TMP/q/src"
cat > "$TMP/q/mcpp.toml" <<'TOML'
[package]
name = "bq"
version = "0.1.0"

[targets.bq]
kind = "bin"
main = "src/main.cpp"

[target.wasm32-emscripten]
runner = ["node"]
TOML
cat > "$TMP/q/src/main.cpp" <<'CPP'
#include <cstdio>
int main() { std::puts("1-2-3"); return 0; }
CPP
cat > "$TMP/q/build.mcpp" <<'CPP'
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

cd "$TMP/q"
MCPP_HOME="$FRESH_HOME" "$MCPP" build --target wasm32-emscripten > fresh-build.log 2>&1 \
    || fail "a fresh \$MCPP_HOME's first build does not build for wasm32-emscripten (row-pin fallback picked the row's own compiler as the host toolchain again)" fresh-build.log
grep -q 'AssertionError' fresh-build.log && fail "emcc.py's assertion is in the fresh-home log" fresh-build.log
grep -q 'host toolchain for build.mcpp: clang' fresh-build.log \
    && fail "the fresh-home host toolchain for build.mcpp was the Web row's own clang" fresh-build.log
[ -f bp-env.txt ] || fail "the build program did not run in the fresh home" fresh-build.log
fresh_host_triple=$(grep '^host=' bp-env.txt | cut -d= -f2)
fresh_target_triple=$(grep '^target=' bp-env.txt | cut -d= -f2)
[ "$fresh_target_triple" = "wasm32-emscripten" ] || fail "fresh-home MCPP_TARGET was '$fresh_target_triple'" bp-env.txt
case "$fresh_host_triple" in wasm32*|"") fail "fresh-home MCPP_HOST was '$fresh_host_triple'" bp-env.txt ;; esac
[ -n "$(find target/wasm32-emscripten -path '*/bin/bq.js' | head -1)" ] || fail "no bin/bq.js in the fresh home" fresh-build.log
MCPP_HOME="$FRESH_HOME" "$MCPP" run --target wasm32-emscripten > fresh-run.log 2>&1 \
    || fail "mcpp run failed in the fresh home" fresh-run.log
grep -qx '1-2-3' fresh-run.log || fail "the program did not print 1-2-3 in the fresh home" fresh-run.log

# ── PHASE 3: the SAME fresh home's native path must still work — a wrong
# fallback (e.g. always overriding the host spec, rather than only when the
# row replaced nothing) would show up here as the native build picking up
# something wasm-shaped instead of a real host compiler. ───────────────────
rm -rf target
MCPP_HOME="$FRESH_HOME" "$MCPP" build > fresh-native-build.log 2>&1 \
    || fail "the native build in the same fresh home regressed" fresh-native-build.log
native_bin=$(find target -path '*/bin/bq' | head -1)
[ -n "$native_bin" ] || fail "no native bin/bq in the same fresh home" fresh-native-build.log
[ "$("$native_bin")" = "1-2-3" ] || fail "the native bin/bq in the same fresh home did not print 1-2-3" fresh-native-build.log

echo "PASS: 657_a_build_program_under_the_web_row_uses_the_host_compiler"
