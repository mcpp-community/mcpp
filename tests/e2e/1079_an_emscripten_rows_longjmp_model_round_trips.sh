#!/usr/bin/env bash
# requires: elf
# 1079 -- `longjmp` is the third `abi` member: the model a WebAssembly
# program's sigsetjmp/siglongjmp cross the boundary by (2026.11.11.1).
#
# With the toolchain's default JavaScript model the pair compiles into a
# stack copy, and a non-local exit over two call levels reads past the
# module's stack and dies at run time (measured: memory access out of bounds)
# while the build and the link succeed -- a failure nothing names. With
# `-sSUPPORT_LONGJMP=wasm` on both the compiles and the link, the pair
# round-trips. A deep exit wants a bigger stack; that is the project's own
# knob (`-sSTACK_SIZE` in ldflags), not an ABI fact, and the engine does not
# render it.
#
# Same self-probe gate as 653/650: `# requires: elf` only, because whether
# THIS machine has the emsdk payload is not a portable capability. Declared
# on a non-Emscripten row the member is refused naming the row's own os.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -30 "$f" 2>/dev/null; done; exit 1; }

have_emsdk=0
for d in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-emsdk/*/emscripten \
         "$HOME"/.xlings/data/xpkgs/xim-x-emsdk/*/emscripten; do
    [[ -x "$d/em++" ]] && have_emsdk=1
done
if [ "$have_emsdk" -ne 1 ] || ! command -v node >/dev/null 2>&1; then
    echo "SKIP: 1079 -- no emsdk payload or node on this machine"
    exit 77
fi

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "lj"
version = "0.1.0"

[targets.lj]
kind = "bin"
main = "src/main.c"

[target.wasm32-emscripten]
runner = ["node"]

[target.'cfg(os = "emscripten")'.abi]
longjmp = "wasm"

[target.'cfg(os = "emscripten")'.build]
ldflags = ["-sSTACK_SIZE=1048576"]
TOML
cat > src/main.c <<'C'
/* Emscripten's libc declares the sigsetjmp/siglongjmp aliases behind the
   feature macros, like every libc; the fixture states the one it needs. */
#define _GNU_SOURCE 1
#include <setjmp.h>
#include <stdio.h>

static jmp_buf home;

static void level_two(void) {
    volatile int kept = 7;
    siglongjmp(home, 1);
    (void)kept;
}

static void level_one(void) { level_two(); }

int main(void) {
    volatile int marker = 0;
    if (sigsetjmp(home, 1) == 0) {
        marker = 1;
        level_one();
        printf("unreachable\n");
        return 1;
    }
    /* The volatile locals of the frames unwound by the non-local exit. */
    if (marker != 1) { printf("marker lost\n"); return 2; }
    printf("round-trip\n");
    return 0;
}
C

"$MCPP" build --target wasm32-emscripten > b.log 2>&1 || fail "the wasm build with longjmp = \"wasm\" failed" b.log
"$MCPP" run --target wasm32-emscripten > run.log 2>&1 || fail "the run failed -- the non-local exit did not round-trip" run.log
grep -q '^round-trip$' run.log || fail "the program did not reach its end" run.log

# The model is a member of Emscripten rows alone: a Linux build that declares
# it is refused naming that fact, not silently satisfied.
cat > mcpp.toml <<'TOML'
[package]
name    = "lj"
version = "0.1.0"

[targets.lj]
kind = "bin"
main = "src/main.c"

[target.'cfg(os = "linux")'.abi]
longjmp = "wasm"
TOML
if "$MCPP" build --target x86_64-linux-musl > refuse.log 2>&1; then
    fail "longjmp = \"wasm\" on a Linux row was accepted"
fi
grep -q "not an Emscripten row" refuse.log || fail "the refusal does not name the row's own os" refuse.log
echo "PASS: 1079 an Emscripten row's longjmp model round-trips"
