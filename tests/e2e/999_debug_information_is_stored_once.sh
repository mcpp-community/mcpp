#!/usr/bin/env bash
# requires: gcc
# 999 -- `split_debuginfo` (design 2026-10-31 §4.1 (b)): under `dev` the units
# compiled in the build directory keep their debug information beside their
# objects, and a program carries a skeleton that names it; an index package's
# units, which enter the global cache and are staged by other projects, keep
# theirs in the object (compressed).
#
# Criteria:
#   A. A dev build: the root's unit has a `.dwo`, the index package's units
#      have none, the program's `.debug_info` is compressed, and gdb reads the
#      root's source through the skeleton.
#   B. A release build writes no `.dwo`.
#   C. `[profile.dev] split_debuginfo = "off"` writes no `.dwo`.
#   D. A value other than "unpacked" and "off" is refused, naming both.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
[ "$(uname -s)" = Linux ] || { echo "SKIP: the split form is checked on Linux (ELF)"; exit 0; }

mkdir -p app/src
cd app
write_manifest() {
    printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[dependencies]\n"compat.zlib" = "1.3.2"\n%s' "$1" > mcpp.toml
}
write_manifest ""
cat > src/main.cpp <<'EOF'
#include <zlib.h>
#include <cstdio>
int main() { std::printf("%s\n", zlibVersion()); }
EOF

# A
"$MCPP" build > a.log 2>&1 || fail "the dev build failed" a.log
dir=$(ls -d target/*/*/ | head -1)
find "$dir/obj" -name 'main*.dwo' | grep -q . || fail "the root's unit has no .dwo" <(find "$dir/obj")
[ -z "$(find "$dir/obj" -path '*compat_zlib*' -name '*.dwo')" ] || fail "an index package's unit was split" <(find "$dir/obj" -name '*.dwo')
program=$(find "$dir/bin" -name app -type f | head -1)
readelf -S -W "$program" | grep -E '\.debug_info' | grep -q ' C ' || fail "the program's .debug_info is not compressed" <(readelf -S -W "$program")
if command -v gdb > /dev/null; then
    gdb -batch -ex 'list main' "$program" 2> /dev/null | grep -q zlibVersion || fail "gdb does not read the source through the skeleton"
fi

# B
rm -rf target
"$MCPP" build --release > b.log 2>&1 || fail "the release build failed" b.log
[ -z "$(find target -name '*.dwo')" ] || fail "a release build wrote .dwo files" <(find target -name '*.dwo')

# C
rm -rf target
write_manifest $'\n[profile.dev]\nsplit_debuginfo = "off"\n'
"$MCPP" build > c.log 2>&1 || fail "the build with split_debuginfo off failed" c.log
[ -z "$(find target -name '*.dwo')" ] || fail "split_debuginfo = \"off\" wrote .dwo files" <(find target -name '*.dwo')

# D
write_manifest $'\n[profile.dev]\nsplit_debuginfo = "packed"\n'
"$MCPP" build > d.log 2>&1 && fail "an unknown split_debuginfo value was accepted" d.log
grep -q '"unpacked" or "off"' d.log || fail "the refusal does not name the values" d.log

echo "OK"
