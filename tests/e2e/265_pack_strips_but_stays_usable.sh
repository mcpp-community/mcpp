#!/usr/bin/env bash
# requires: pack python3
# 265_pack_strips_but_stays_usable.sh — `mcpp pack` ships stripped artifacts,
# and stripping must not make them unusable.
#
# THE ONE THAT BITES: A STATIC ARCHIVE CANNOT BE `--strip-all`ed.
#
# `strip` with no shape argument defaults to strip-all, and on a `.a` that
# removes the ARCHIVE SYMBOL INDEX. The package then fails at the CONSUMER's
# link with
#
#   ld: libmathkit.a: error adding symbols: archive has no index; run ranlib to add one
#
# — a message that names neither `strip` nor the publisher, arriving on a
# machine the publisher does not have. Measured: `--strip-debug` keeps the
# index (2988 → 1244 bytes) and the consumer links and runs. So the packer
# uses dh_strip's division, and this test consumes BOTH shapes rather than
# inspecting them: "the archive still has an index" is a proxy, "a program
# linked against it prints the right number" is the thing.
#
# The three configuration channels are pinned from BOTH sides — a test that
# only checks the default cannot tell "the switch works" from "there is no
# switch".
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
EOF

# `file` is not required by the harness; ask the ELF itself instead.
has_symtab() {
python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\x7fELF' or d[4] != 2:
    print("no"); raise SystemExit
shoff, = struct.unpack_from('<Q', d, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3A)
names_off, = struct.unpack_from('<Q', d, shoff + shstrndx * shentsize + 0x18)
found = "no"
for i in range(shnum):
    base = shoff + i * shentsize
    name_idx, sh_type = struct.unpack_from('<II', d, base)
    end = d.index(b'\0', names_off + name_idx)
    name = d[names_off + name_idx:end].decode()
    if sh_type == 2 or name.startswith('.debug_'):   # SHT_SYMTAB
        found = "yes"
print(found)
PY
}

consume() {   # <package dir> <expected token> — build a program against it and run
    local pkg="$1" want="$2" tag="$3"
    # Through a named *_HOST variable, not interpolated inline: the manifest
    # below is FILE CONTENT, and on Git Bash a shell-spelled /tmp/... path is
    # read by a native mcpp.exe as "root of the current drive". 00_fixture_path
    # _hygiene enforces the naming so the conversion is visible at the use site.
    local PKG_HOST
    PKG_HOST="$(host_path "$pkg")"
    rm -rf "$TMP/consumer"
    mkdir -p "$TMP/consumer/src"
    cat > "$TMP/consumer/src/main.cpp" <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
    cat > "$TMP/consumer/mcpp.toml" <<EOF
[package]
name    = "consumer"
version = "0.1.0"
[dependencies]
mathkit = { path = "$PKG_HOST" }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF
    ( cd "$TMP/consumer" && "$MCPP" run > run.log 2>&1 ) || {
        cat "$TMP/consumer/run.log"
        echo "FAIL[$tag]: a consumer cannot use the stripped package"; exit 1; }
    grep -q "$want" "$TMP/consumer/run.log" || {
        cat "$TMP/consumer/run.log"; echo "FAIL[$tag]: wrong answer"; exit 1; }
}

cd mathkit

# ── 1. static: stripped, and STILL LINKABLE ────────────────────────────────
rm -rf target/dist
"$MCPP" pack mathkit > pack-a.log 2>&1 || { cat pack-a.log; echo "FAIL: static pack"; exit 1; }
grep -q "Stripped" pack-a.log || { cat pack-a.log; echo "FAIL: the static leg was not stripped"; exit 1; }
pkg_a="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
consume "$pkg_a" "ok=42" "static"
echo "  static archive: stripped and still linkable"

# ── 2. shared: stripped, and STILL LOADABLE ────────────────────────────────
rm -rf target/dist
"$MCPP" pack mathkit-shared > pack-b.log 2>&1 || { cat pack-b.log; echo "FAIL: shared pack"; exit 1; }
grep -q "Stripped" pack-b.log || { cat pack-b.log; echo "FAIL: the shared leg was not stripped"; exit 1; }
pkg_b="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
so="$(find "$pkg_b/lib" -name 'libmathkit-shared.so' -type f | head -1)"
[[ -n "$so" ]] || { echo "FAIL: no .so"; exit 1; }
# `--strip-unneeded` must leave `.dynsym` — it IS the export list. If this
# regressed to `--strip-all` the link below would still work (ld reads
# `.dynsym`), so the run is what proves it.
[[ "$(has_symtab "$so")" == "no" ]] || {
    echo "FAIL: the shared library still carries a symbol/debug table after stripping"; exit 1; }
consume "$pkg_b" "ok=42" "shared"
echo "  shared library: stripped and still loadable"

# ── 3. the OTHER side of every switch ──────────────────────────────────────
rm -rf target/dist
"$MCPP" pack mathkit-shared --no-strip > pack-c.log 2>&1 || { cat pack-c.log; echo "FAIL: --no-strip pack"; exit 1; }
grep -q "Stripped" pack-c.log && { cat pack-c.log; echo "FAIL: --no-strip stripped anyway"; exit 1; }
pkg_c="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
so_c="$(find "$pkg_c/lib" -name 'libmathkit-shared.so' -type f | head -1)"
[[ "$(has_symtab "$so_c")" == "yes" ]] || {
    echo "FAIL: --no-strip produced an artifact with no symbols — the flag did nothing"
    exit 1; }
echo "  --no-strip: symbols kept"

rm -rf target/dist
cp mcpp.toml mcpp.toml.bak
printf '\n[pack]\nstrip = false\n' >> mcpp.toml
"$MCPP" pack mathkit-shared > pack-d.log 2>&1 || { cat pack-d.log; echo "FAIL: [pack] strip pack"; exit 1; }
grep -q "Stripped" pack-d.log && { cat pack-d.log; echo "FAIL: [pack] strip = false was ignored"; exit 1; }
mv mcpp.toml.bak mcpp.toml
echo "  [pack] strip = false: honoured"

# ── 4. --debug-symbols keeps the symbols, beside the artifact ──────────────
rm -rf target/dist
"$MCPP" pack mathkit-shared --debug-symbols dbg > pack-e.log 2>&1 || {
    cat pack-e.log; echo "FAIL: --debug-symbols pack"; exit 1; }
# PER TRIPLE, mirroring `lib/<triple>/`. A fat package's legs share an artifact
# NAME (`libmathkit-shared.so` for both a gnu and a musl leg is the normal case),
# so a flat debug directory would have the second leg overwrite the first — and
# the first artifact's `.gnu_debuglink` would then resolve to the other target's
# symbols, silently. Asserting the layout is what keeps that from regressing.
dbgfile="$(find dbg -name 'libmathkit-shared.so.debug' | head -1)"
[[ -n "$dbgfile" ]] || { ls -R dbg 2>&1; echo "FAIL: no separated debug file"; exit 1; }
[[ "$dbgfile" == dbg/*/libmathkit-shared.so.debug ]] || {
    echo "FAIL: the debug file is not under a per-triple directory: $dbgfile"
    echo "      A fat package's legs share an artifact name; a flat layout loses one."
    exit 1; }
[[ "$(has_symtab "$dbgfile")" == "yes" ]] || {
    echo "FAIL: the separated debug file carries no debug information"; exit 1; }
pkg_e="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
so_e="$(find "$pkg_e/lib" -name 'libmathkit-shared.so' -type f | head -1)"
[[ "$(has_symtab "$so_e")" == "no" ]] || {
    echo "FAIL: --debug-symbols left the debug information in the shipped artifact too"
    exit 1; }
# The link back. Without it a debugger has the file and no way to find it.
python3 - "$so_e" <<'PY' || { echo "FAIL: no .gnu_debuglink in the stripped artifact"; exit 1; }
import struct, sys
d = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<Q', d, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3A)
names_off, = struct.unpack_from('<Q', d, shoff + shstrndx * shentsize + 0x18)
names = []
for i in range(shnum):
    idx, = struct.unpack_from('<I', d, shoff + i * shentsize)
    end = d.index(b'\0', names_off + idx)
    names.append(d[names_off + idx:end].decode())
raise SystemExit(0 if '.gnu_debuglink' in names else 1)
PY
echo "  --debug-symbols: separated, and the artifact points back at it"

echo "PASS: packed artifacts are stripped, still usable, and every switch works from both sides"
