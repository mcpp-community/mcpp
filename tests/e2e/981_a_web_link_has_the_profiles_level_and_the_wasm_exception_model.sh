#!/usr/bin/env bash
# requires: elf
# #851 (design 2026-10-29 §6): emcc runs wasm-opt and minifies the launcher
# AT THE LINK, so the profile's optimisation level reaches the link; and
# `[target.<sel>.abi] exceptions = "wasm"` selects WebAssembly's own exception
# handling (`-fwasm-exceptions`), while `true` keeps the JavaScript model.
# Same self-probe gate as 653: no emsdk payload, no test.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

have_emsdk=0
for d in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-emsdk/*/emscripten \
         "$HOME"/.xlings/data/xpkgs/xim-x-emsdk/*/emscripten; do
    [[ -x "$d/em++" ]] && have_emsdk=1
done
if [ "$have_emsdk" -ne 1 ]; then
    echo "SKIP: 981 -- no emsdk payload installed"
    echo "PASS: 981 (skipped)"
    exit 0
fi

mkdir -p "$TMP/app/src"
cd "$TMP/app"
write() {   # $1 = the abi table's value, or empty for none
    cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[target.wasm32-emscripten]
runner = ["node"]
TOML
    if [ -n "$1" ]; then printf '\n[target.%s.abi]\nexceptions = %s\n' "'cfg(os = \"emscripten\")'" "$1" >> mcpp.toml; fi
}
cat > src/main.cpp <<'CPP'
import std;
int main() {
    try { throw std::runtime_error("boom"); }
    catch (std::exception const& e) { std::println("caught: {}", e.what()); }
}
CPP
ninja_of() { dirname "$(ls -t $(find target/wasm32-emscripten -name build.ninja -not -path '*/.build-mcpp/*') | head -1)"; }

echo "== 981/1: a release link carries the profile's level, a dev link -O0 =="
write ''
"$MCPP" build --release --target wasm32-emscripten > rel.log 2>&1 || fail "release build failed" rel.log
grep -E '^ldflags' "$(ninja_of)/build.ninja" | grep -qE -- '(^| )-O2( |$)' \
    || fail "the release link carries no -O2" "$(ninja_of)/build.ninja"
"$MCPP" build --target wasm32-emscripten > dev.log 2>&1 || fail "dev build failed" dev.log
grep -E '^ldflags' "$(ninja_of)/build.ninja" | grep -qE -- '(^| )-O0( |$)' \
    || fail "the dev link carries no -O0" "$(ninja_of)/build.ninja"
echo "  ok"

echo "== 981/2: exceptions = \"wasm\" compiles and links with -fwasm-exceptions, and catches =="
write '"wasm"'
"$MCPP" build --target wasm32-emscripten > wasm.log 2>&1 || fail "the wasm-model build failed" wasm.log
n="$(ninja_of)/build.ninja"
grep -E '^cxxflags' "$n" | grep -q -- '-fwasm-exceptions' || fail "-fwasm-exceptions is not compiled with" "$n"
grep -E '^ldflags' "$n" | grep -q -- '-fwasm-exceptions' || fail "-fwasm-exceptions is not linked with" "$n"
grep -E '^cxxflags' "$n" | grep -qE -- '(^| )-fexceptions( |$)' && fail "the JS model is also selected" "$n"
"$MCPP" run --target wasm32-emscripten > wasm-run.log 2>&1 || fail "the wasm-model program did not run" wasm-run.log
grep -q '^caught: boom$' wasm-run.log || fail "the wasm-model program did not catch" wasm-run.log
echo "  ok"

echo "== 981/3: true keeps the JavaScript model =="
write 'true'
"$MCPP" build --target wasm32-emscripten > js.log 2>&1 || fail "the js-model build failed" js.log
n="$(ninja_of)/build.ninja"
grep -E '^cxxflags' "$n" | grep -qE -- '(^| )-fexceptions( |$)' || fail "true no longer selects -fexceptions" "$n"
grep -E '^cxxflags' "$n" | grep -q -- '-fwasm-exceptions' && fail "true selects the wasm model" "$n"
echo "  ok"

echo "== 981/4: another value is refused, naming the values =="
write '"seh"'
if "$MCPP" build --target wasm32-emscripten > bad.log 2>&1; then fail "exceptions = \"seh\" was accepted" bad.log; fi
grep -qF '"wasm" or "js"' bad.log || fail "the refusal does not name the values" bad.log
echo "  ok"

echo "PASS: 981"
