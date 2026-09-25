#!/usr/bin/env bash
# requires: llvm elf unix-shell
# mcpp#696: a link whose C library comes from the dependency graph searches no
# library directory of the host.
#
# Before the fix clang derived `/usr/lib/x86_64-linux-gnu` and its siblings for
# a `x86_64-linux-musl` link over openkal-musl, so `-lm` was answered by the
# HOST's glibc archive and its objects were linked into a musl image, with no
# diagnostic. The graph link now carries `--sysroot` naming an empty directory
# in the build directory, which removes every such directory.
#
# Four legs; the second is the one that separates the engines:
#
#   A. openkal-musl 0.19.2 ships musl's eight empty archives (`libm.a` among
#      them), through openkal-llvm-runtime 0.15.2, which pins it. `-lm` is
#      answered by the graph, the link carries the empty sysroot, and the
#      program runs.
#   B. openkal-musl 0.19.1 (runtime 0.15.1) has no `libm.a`. With the host's
#      directories gone, `-lm` is unanswered: the build fails with the linker's
#      own message and mcpp's note naming the release that answers it. The
#      released engine links this leg successfully, from the host's libm.
#   C. The report's own case: aarch64-linux-musl, where the host's `libm.a`
#      is an x86_64 linker script. It links, and runs under qemu-aarch64.
#   D. A host directory given in `ldflags` is refused by the hermetic check,
#      which names it.
#
# The runtime pins openkal-musl exactly, so each leg names the pair that
# belongs together: a root that pins another openkal-musl is refused as
# irreconcilable before anything is linked.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
TARGET=x86_64-linux-musl

fail() { [ -n "$2" ] && cat "$2"; echo "FAIL: $1"; exit 1; }

make_project() {
    local dir="$1" musl="$2" runtime="$3" ldflags="${4:-\"-lm\"}"
    mkdir -p "$dir/src"
    cat > "$dir/mcpp.toml" <<TOML
[package]
name    = "lmprobe"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[build]
ldflags = [$ldflags]

[targets.lmprobe]
kind = "bin"
main = "src/main.c"

[dependencies]
openkal-musl         = "$musl"
openkal-llvm-runtime = "$runtime"
TOML
    cat > "$dir/src/main.c" <<'C'
#include <math.h>
#include <stdio.h>
int main(void) {
    volatile double a = 1.0, b = 2.0;
    printf("%g\n", fmax(a, b));
    return fmax(a, b) == 2.0 ? 0 : 1;
}
C
}

skip_if_unreachable() {
    if grep -qE 'not found in the synced index|install_packages failed' "$1"; then
        echo "SKIP: the openkal packages are not reachable from here"
        exit 0
    fi
}

# ── A. the graph answers -lm ────────────────────────────────────────────────
make_project "$work/a" "0.19.2" "0.15.2"
cd "$work/a"
if ! "$MCPP" build --target "$TARGET" --verbose > a.log 2>&1; then
    skip_if_unreachable a.log
    fail "with openkal-musl 0.19.2, -lm must be answered by the graph" a.log
fi
grep -q -- '--sysroot=[^ ]*graph-sysroot' a.log \
    || fail "the graph link does not carry the empty sysroot" a.log
out=$("$MCPP" run --target "$TARGET" 2>&1) || fail "the program did not run: $out"
printf '%s\n' "$out" | grep -qx '2' || fail "unexpected output: $out"
echo "  ok: -lm is answered by openkal-musl's own archive, and the program runs"

# ── B. nothing in the graph answers -lm ─────────────────────────────────────
make_project "$work/b" "0.19.1" "0.15.1"
cd "$work/b"
if "$MCPP" build --target "$TARGET" > b.log 2>&1; then
    fail "-lm was answered although nothing in the graph provides it: the host's library directories are still searched" b.log
fi
skip_if_unreachable b.log
grep -q 'unable to find library -lm' b.log \
    || fail "the failure is not the unanswered -lm" b.log
grep -q 'openkal-musl 0.19.2' b.log \
    || fail "the note does not name the release that answers -lm" b.log
echo "  ok: an unanswered -lm fails, and the note names openkal-musl 0.19.2"

# ── C. the report's case, on aarch64 ────────────────────────────────────────
make_project "$work/c" "0.19.2" "0.15.2"
cd "$work/c"
"$MCPP" build --target aarch64-linux-musl > c.log 2>&1 \
    || fail "with openkal-musl 0.19.2, -lm must link on aarch64-linux-musl" c.log
bin=$(find target -path '*aarch64*' -name lmprobe -type f | head -1)
[ -n "$bin" ] || fail "no aarch64 lmprobe was produced" c.log
runner="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
if [ -z "$runner" ]; then
    echo "  SKIP  no aarch64 emulator here; the link was checked, not the run"
else
    ran=$("$runner" "$bin" 2>&1) || fail "non-zero exit under $(basename "$runner"): $ran"
    [ "$ran" = "2" ] || fail "unexpected output under emulation: $ran"
    echo "  ok: aarch64-linux-musl links -lm from the graph and runs under $(basename "$runner")"
fi

# ── D. a host directory given in ldflags ────────────────────────────────────
make_project "$work/d" "0.19.2" "0.15.2" '"-L/usr/lib", "-lm"'
cd "$work/d"
if "$MCPP" build --target "$TARGET" > d.log 2>&1; then
    fail "a graph link was allowed to search /usr/lib" d.log
fi
grep -q 'hermetic link check failed' d.log \
    || fail "the refusal is not the hermetic check's" d.log
grep -q '/usr/lib' d.log || fail "the refusal does not name the directory" d.log
echo "  ok: a host directory in ldflags is refused, by name"

echo "PASS: a graph link searches no host directory"
