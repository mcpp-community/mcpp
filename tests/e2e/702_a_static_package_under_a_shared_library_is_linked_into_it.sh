#!/usr/bin/env bash
# requires: unix-shell python3
# 702 -- one static package, one image (#646 F1).
#
# A dependency's shared library was linked from its own package's objects only.
# A static package beneath it went into the PROGRAM, and on ELF the library
# bound to the program's copy at run time. That worked for that program alone:
# measured on Linux, the same `libfw.so` refused `-Wl,-z,defs` and a host that
# did not link the package could not load it (`undefined symbol: x_answer`);
# Mach-O and PE resolve every reference at link time, so the library did not
# link there at all.
#
# Three legs:
#   A. fw (shared) over x (static): x is linked into libfw, which then loads
#      from a host that never linked x (Python's ctypes, RTLD_NOW);
#   B. the program depends on x as well: no single image can own x. On ELF the
#      build proceeds as it always did and says so (`--strict` fails); on
#      Mach-O the build is refused before compiling, naming the remedy;
#   C. the remedy the message names, `linkage = "shared"` on x, builds under
#      `--strict` and runs.
set -e

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) echo "skip: the PE legs of 702 are not wired on this shard"; exit 0 ;;
esac

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

PYTHON="$(command -v python3 || command -v python || true)"
[[ -n "$PYTHON" ]] || fail "no python to load the library from a foreign host"

MACHO=0
[ "$(uname -s)" = "Darwin" ] && MACHO=1
SO=so
[ $MACHO = 1 ] && SO=dylib

cd "$TMP"
mkdir -p x/src fw/src app/src
cat > x/mcpp.toml <<'TOML'
[package]
name    = "x"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.x]
kind = "lib"
TOML
cat > x/src/x.c <<'C'
int x_answer(void) { return 41; }
C
cat > fw/mcpp.toml <<'TOML'
[package]
name    = "fw"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.fw]
kind = "shared"

[dependencies]
x = { path = "../x" }
TOML
cat > fw/src/fw.c <<'C'
extern int x_answer(void);
__attribute__((visibility("default"))) int fw_answer(void) { return x_answer() + 1; }
C

write_app() {   # $1 = extra [dependencies] lines, $2 = main body
    cat > app/mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[dependencies]
fw = { path = "../fw" }
$1
TOML
    printf '%s\n' "$2" > app/src/main.cpp
}

# ── A. x belongs in libfw ────────────────────────────────────────────────────
write_app '' 'extern "C" int fw_answer(void);
int main() { return fw_answer() == 42 ? 0 : 1; }'
cd app
"$MCPP" build > a.log 2>&1 || fail "a shared library over a static package did not build" a.log
dir=$(ls -d target/*/*/ | head -1)
"$dir/bin/app" || fail "the program over libfw exited non-zero" a.log
lib="$dir/bin/libfw.$SO"
[ -f "$lib" ] || fail "no libfw.$SO" a.log
loaded=$("$PYTHON" - "$lib" <<'PY' 2>&1
import ctypes, os, sys
lib = ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW)
print(lib.fw_answer())
PY
) || fail "a host that never linked x cannot load libfw: $loaded" a.log
[ "$loaded" = "42" ] || fail "expected 42 from libfw in a foreign host, got: $loaded"
echo "ok: a static package reachable only through a shared library is linked into it"
cd ..

# ── B. the program reaches x too ────────────────────────────────────────────
write_app 'x = { path = "../x" }' 'extern "C" int fw_answer(void);
extern "C" int x_answer(void);
int main() { return fw_answer() == 42 && x_answer() == 41 ? 0 : 1; }'
cd app
rm -rf target
if [ $MACHO = 1 ]; then
    if "$MCPP" build > b.log 2>&1; then
        fail "a static package in two Mach-O images was not refused" b.log
    fi
    grep -q "a static package is linked into more than one image" b.log \
        || fail "the refusal does not say what it refuses" b.log
    grep -q "'mcpplibs.x' is reached by the program, 'fw'" b.log \
        || fail "the refusal does not name the package and its images" b.log
    grep -q 'linkage = "shared"' b.log || fail "the refusal does not name the remedy" b.log
    echo "ok: a static package in two Mach-O images is refused before compiling"
else
    "$MCPP" build > b.log 2>&1 || fail "the two-image graph no longer builds on ELF" b.log
    grep -q "a static package is reachable from more than one image" b.log \
        || fail "the ELF build does not report the two-image package" b.log
    grep -q "'mcpplibs.x' is reached by the program, 'fw'" b.log \
        || fail "the report does not name the package and its images" b.log
    dir=$(ls -d target/*/*/ | head -1)
    "$dir/bin/app" || fail "the two-image program exited non-zero" b.log
    rm -rf target
    if "$MCPP" build --strict > b-strict.log 2>&1; then
        fail "--strict accepted a static package in two images" b-strict.log
    fi
    echo "ok: a static package in two ELF images builds as before and is reported"
fi
cd ..

# ── C. the remedy the message names ─────────────────────────────────────────
write_app 'x = { path = "../x", linkage = "shared" }' 'extern "C" int fw_answer(void);
extern "C" int x_answer(void);
int main() { return fw_answer() == 42 && x_answer() == 41 ? 0 : 1; }'
cd app
rm -rf target
"$MCPP" build --strict > c.log 2>&1 || fail "linkage = \"shared\" on x did not build under --strict" c.log
grep -q "more than one image" c.log && fail "the shared form still reports the two-image package" c.log
dir=$(ls -d target/*/*/ | head -1)
[ -f "$dir/bin/libx.$SO" ] || fail "no libx.$SO under linkage = \"shared\"" c.log
"$dir/bin/app" || fail "the program over the shared x exited non-zero" c.log
echo "ok: linkage = \"shared\" gives the package one image"

echo "PASS: 702 a static package under a shared library is linked into it"
