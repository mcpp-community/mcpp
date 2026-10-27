#!/usr/bin/env bash
# requires: unix-shell python3
# 956 -- a single file (SPEC-014 §5.2): `--pkg-format single-file` writes one
# executable. A native program's file is the engine's launcher carrying the
# staged tree as a zstd frame, extracted once into the user's cache under the
# payload's sha256; an image's file is the platform's carrier, which runs the
# image from memory.
#
# Criteria:
#   A. The native file runs the program with its arguments and exit status;
#      the first run extracts the tree once, a second run extracts nothing.
#   B. On Linux, `--linkage static` makes a file with no interpreter.
#   C. Refused before a build: `--compress xz` for a single file.
#   D. (llvm, an ok-image row) An image's single file runs, and writes nothing
#      into the cache.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
# The user's cache, isolated: `$XDG_CACHE_HOME` on Linux; on macOS the cache is
# `~/Library/Caches`, so the files run with a home of their own (mcpp keeps
# the real one).
if [ "$(uname -s)" = Darwin ]; then
    CACHE="$TMP/home/Library/Caches"
    mkdir -p "$CACHE"
    run_file() { HOME="$TMP/home" "$@"; }
else
    export XDG_CACHE_HOME="$TMP/cache"
    CACHE="$XDG_CACHE_HOME"
    run_file() { "$@"; }
fi

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "one"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
#include <cstdio>
int main(int argc, char** argv) { std::printf("one file: %d args, last %s\n", argc, argv[argc - 1]); return 3; }
CPP

# A
"$MCPP" pack --pkg-format single-file > a.log 2>&1 || fail "A: pack" a.log
f=$(python3 - <<'PY'
import glob, os
c = [p for p in glob.glob("target/dist/one-0.1.0-*") if os.path.isfile(p) and os.access(p, os.X_OK)]
print(c[0] if c else "")
PY
)
[ -n "$f" ] || fail "A: no single file under target/dist" a.log
set +e; run_file "$f" alpha beta > run1.log 2>&1; rc=$?; set -e
[ "$rc" = 3 ] || fail "A: the exit status is $rc, not the program's 3" run1.log
grep -q "one file: 3 args, last beta" run1.log || fail "A: the program's output" run1.log
dirs=$(ls "$CACHE/mcpp/single-file" | grep -v '^\.' | wc -l | tr -d ' ')
[ "$dirs" = 1 ] || fail "A: the first run left $dirs trees in the cache"
stamp=$(ls --full-time "$CACHE/mcpp/single-file"/*/.mcpp-complete 2>/dev/null || stat "$CACHE"/mcpp/single-file/*/.mcpp-complete)
set +e; run_file "$f" gamma > run2.log 2>&1; set -e
grep -q "last gamma" run2.log || fail "A: the second run" run2.log
stamp2=$(ls --full-time "$CACHE/mcpp/single-file"/*/.mcpp-complete 2>/dev/null || stat "$CACHE"/mcpp/single-file/*/.mcpp-complete)
[ "$stamp" = "$stamp2" ] || fail "A: the second run extracted the tree again"
echo "ok: a native single file runs, extracting once"

# B
if [ "$(uname -s)" = Linux ]; then
    "$MCPP" pack --pkg-format single-file --linkage static -o one-static > b.log 2>&1 || fail "B: --linkage static" b.log
    python3 - target/dist/one-static <<'PY' || fail "B: the static file has an interpreter" b.log
import struct, sys
data = open(sys.argv[1], "rb").read()
assert data[:4] == b"\x7fELF"
phoff, = struct.unpack_from("<Q", data, 32)
phentsize, phnum = struct.unpack_from("<HH", data, 54)
for i in range(phnum):
    ptype, = struct.unpack_from("<I", data, phoff + i * phentsize)
    assert ptype != 3, "PT_INTERP"
PY
    set +e; target/dist/one-static z > b-run.log 2>&1; set -e
    grep -q "last z" b-run.log || fail "B: the static single file" b-run.log
    echo "ok: --linkage static makes a file with no interpreter"
fi

# C
if "$MCPP" pack --pkg-format single-file --compress xz > c.log 2>&1; then fail "C: xz was accepted" c.log; fi
grep -q "zstd" c.log || fail "C: the refusal does not name the codec" c.log
echo "ok: a single file's payload is zstd"

# D
case "$(uname -m)" in
    x86_64|amd64)  T=x86_64-openkal ;;
    aarch64|arm64) T=aarch64-openkal ;;
    *) T= ;;
esac
if [ -n "$T" ] && command -v python3 > /dev/null && "$MCPP" toolchain list --format json 2>/dev/null | grep -q llvm; then
    mkdir -p "$TMP/img/src"
    cd "$TMP/img"
    printf '[package]\nname    = "img"\nversion = "0.1.0"\n' > mcpp.toml
    printf 'import std;\nint main(int argc, char**) { std::println("image file: {} args", argc); }\n' > src/main.cpp
    "$MCPP" pack --target "$T" --pkg-format single-file > d.log 2>&1 || fail "D: pack" d.log
    g=$(ls target/dist/img-0.1.0-* | head -1)
    before=$(find "$CACHE" -type f | wc -l)
    run_file "$g" x y > d-run.log 2>&1 || fail "D: the image's single file" d-run.log
    grep -q "image file: 3 args" d-run.log || fail "D: the output" d-run.log
    after=$(find "$CACHE" -type f | wc -l)
    [ "$before" = "$after" ] || fail "D: the image's single file wrote into the cache"
    echo "ok: an image's single file runs from memory"
fi

echo "PASS: 956 a single file runs without unpacking"
