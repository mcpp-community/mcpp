#!/usr/bin/env bash
# M3: on wasm32-emscripten, which units receive the long-jump model, under the
# row's default and under each statement. Prints, never asserts.
. "$(dirname "$0")/common.sh"
export MCPP_HOME="$PROBE_TMP/mcpp-home"
fetch_mcpp

W="$PROBE_TMP/m3"; rm -rf "$W"; mkdir -p "$W"

jump_c() {
cat > "$1" <<'C'
#include <setjmp.h>
static jmp_buf where;
static void fail(int code) { longjmp(where, code); }
int jumped(int code) {
    if (setjmp(where)) return 1;
    fail(code);
    return 0;
}
C
}
main_cpp() {
cat > "$1" <<'CPP'
#include <cstdio>
#include <stdexcept>
extern "C" int jumped(int code);
int main() {
    try {
        if (jumped(7) != 1) throw std::runtime_error("longjmp did not come back");
    } catch (const std::exception& error) {
        std::puts(error.what());
        return 1;
    }
    std::puts("ok");
    return 0;
}
CPP
}

# $1 = name, $2 = where the C unit is (root|dep), $3 = abi table body or empty
variant() {
    local name="$1" where="$2" abi="$3" d="$W/$1"
    mkdir -p "$d/app/src"
    main_cpp "$d/app/src/main.cpp"
    {
        printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n'
        if [ "$where" = dep ]; then printf '\n[dependencies]\njumplib = { path = "../jumplib" }\n'
        else printf '\n[build]\nsources = ["src/*.c"]\n'; fi
        if [ -n "$abi" ]; then printf "\n[target.'cfg(os = \"emscripten\")'.abi]\n%s\n" "$abi"; fi
    } > "$d/app/mcpp.toml"
    if [ "$where" = dep ]; then
        mkdir -p "$d/jumplib/src"
        jump_c "$d/jumplib/src/jump.c"
        printf '[package]\nname = "jumplib"\nversion = "0.1.0"\n\n[targets.jumplib]\nkind = "lib"\n\n[build]\nsources = ["src/*.c"]\n' > "$d/jumplib/mcpp.toml"
    else
        jump_c "$d/app/src/jump.c"
    fi
    say "M3 $name: C unit in the $where package; abi: ${abi:-<none stated>}"
    ( cd "$d/app" && "$MCPP" build --target wasm32-emscripten -v > "$d/build.log" 2>&1; echo "[exit $?]" >> "$d/build.log" )
    echo "-- the words of the C unit's compile that concern the model:"
    grep -E 'jump\.c' "$d/build.log" | grep -E -- ' -c ' | head -2 \
        | grep -oE -- '(emcc|em\+\+|clang[^ ]*)|-fwasm-exceptions|-fexceptions|-sSUPPORT_LONGJMP=[a-z]+|-pthread' | tr '\n' ' '
    echo
    echo "-- the words of the C++ unit's compile:"
    grep -E 'main\.cpp' "$d/build.log" | grep -E -- ' -c ' | head -1 \
        | grep -oE -- '-fwasm-exceptions|-fexceptions|-sSUPPORT_LONGJMP=[a-z]+' | tr '\n' ' '
    echo
    echo "-- the words of the link:"
    grep -E -- '-o bin/app' "$d/build.log" | head -1 \
        | grep -oE -- '-fwasm-exceptions|-fexceptions|-sSUPPORT_LONGJMP=[a-z]+' | tr '\n' ' '
    echo
    echo "-- errors and the end of the build:"
    grep -nE 'error|undefined symbol|emscripten_longjmp' "$d/build.log" | head -8
    tail -n 4 "$d/build.log"
    local js; js=$(find "$d/app/target" -name 'app.js' 2>/dev/null | head -1)
    if [ -n "$js" ] && command -v node >/dev/null; then echo "-- run:"; node "$js"; echo "[run exit $?]"; fi
}

node --version 2>/dev/null
variant v1-root-default   root ''
variant v2-root-exc-wasm  root 'exceptions = "wasm"'
variant v3-root-both      root 'exceptions = "wasm"
longjmp = "wasm"'
variant v4-dep-default    dep  ''
variant v5-dep-exc-wasm   dep  'exceptions = "wasm"'
variant v6-dep-both       dep  'exceptions = "wasm"
longjmp = "wasm"'
variant v7-dep-js         dep  'exceptions = "js"'

say "M3: the first failing build log in full (tail 60)"
for v in v1-root-default v4-dep-default v6-dep-both; do
    echo "--- $v"; tail -n 60 "$W/$v/build.log" | cut -c1-600
done
