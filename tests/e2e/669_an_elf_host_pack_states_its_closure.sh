#!/usr/bin/env bash
# requires: elf gcc
# 669 -- #634 A3 and A4 on the ELF host row.
#
# A4: an ELF shared library built without a declared `soname` records its file
# name as DT_SONAME; its consumer's DT_NEEDED is the same name it was before,
# and a declared `soname` keeps its value.
#
# A3: the host row keeps its loader-traced closure, and the stage manifest
# gains the `needs` lines every other row writes: a bundled library at its
# staged path, a library the target provides as `platform`. A name the loader
# finds no file for used to be dropped, so an archive that could not start
# said `closure = walked`; now `dir` refuses naming it.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
TAB=$(printf '\t')
soname_of() { readelf -d "$1" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p'; }

cd "$TMP"
mkdir -p fw/src named/src app/src
cat > fw/mcpp.toml <<'EOF'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
EOF
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > fw/src/fw.cppm

cat > named/mcpp.toml <<'EOF'
[package]
namespace = "demo"
name      = "named"
version   = "0.1.0"
[targets.named]
kind   = "shared"
soname = "libnamed.so.1"
EOF
printf 'export module named;\nexport int named_anchor() { return 1; }\n' > named/src/named.cppm

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[toolchain]
linux = "gcc@16.1.0"
[dependencies]
demo.fw    = { path = "../fw", linkage = "shared" }
demo.named = { path = "../named" }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import fw;
import named;
int main() { std::puts("1-2-3"); return fw_anchor() == 41 && named_anchor() == 1 ? 0 : 1; }
EOF
cd app

# ── 1. A4: the default SONAME, and the declared one ───────────────────────
"$MCPP" build > build.log 2>&1 || fail "mcpp build failed" build.log
bin=$(dirname "$(find target -path '*/bin/app' -type f | head -1)")
[ -f "$bin/libfw.so" ] || fail "no bin/libfw.so" build.log
[ "$(soname_of "$bin/libfw.so")" = "libfw.so" ] \
    || fail "libfw.so carries SONAME '$(soname_of "$bin/libfw.so")', not its file name" build.log
readelf -d "$bin/app" | grep -q 'NEEDED.*\[libfw.so\]' \
    || fail "the consumer no longer needs libfw.so by its file name" build.log
named=$(find "$bin" -maxdepth 1 -name 'libnamed.so' -type f | head -1)
[ -n "$named" ] || fail "no bin/libnamed.so" build.log
[ "$(soname_of "$named")" = "libnamed.so.1" ] \
    || fail "the declared soname became '$(soname_of "$named")'" build.log
echo "  ok: libfw.so records its file name; a declared soname keeps libnamed.so.1"

# ── 2. A3: the manifest states the closure the tree carries ───────────────
"$MCPP" pack --format dir > dir.log 2>&1 || fail "mcpp pack --format dir failed" dir.log
tree=$(find target/dist -mindepth 1 -maxdepth 1 -type d -name 'app-0.1.0-*' | head -1)
[ -n "$tree" ] || fail "no staged tree" dir.log
manifest="$tree.stage-manifest"
[ "$(sed -n '1p' "$manifest")" = "closure = walked" ] \
    || fail "the manifest's first line is not 'closure = walked'" "$manifest"
[ -f "$tree/lib/libfw.so" ] || fail "lib/libfw.so is not bundled" dir.log
grep -qxF "needs${TAB}libfw.so${TAB}lib/libfw.so" "$manifest" \
    || fail "the manifest does not state libfw.so at lib/libfw.so" "$manifest"
grep -qxF "needs${TAB}libc.so.6${TAB}platform" "$manifest" \
    || fail "the manifest does not state libc.so.6 as the platform's" "$manifest"
# The needs block sits between the header and the file list.
awk 'NR==1{next} /^needs\t/{ if (files) bad=1; next } { files=1 } END{ exit bad }' "$manifest" \
    || fail "a needs line follows a file line" "$manifest"
"$tree/bin/app" > run.log 2>&1 || fail "the staged program does not run" run.log
grep -q '^1-2-3$' run.log || fail "the staged program did not print 1-2-3" run.log
echo "  ok: needs lines for the bundled and the platform libraries; the tree runs"

# ── 3. a library the loader cannot find refuses the archive ───────────────
cd "$TMP"
mkdir -p ext/lib extapp/src
printf 'int ext_value() { return 1; }\n' > ext/ext.cpp
g++ -shared -fPIC -Wl,-soname,libext.so -o ext/lib/libext.so ext/ext.cpp
cat > extapp/mcpp.toml <<EOF
[package]
name    = "extapp"
version = "0.1.0"
[toolchain]
linux = "gcc@16.1.0"
[build]
ldflags = ["-L$TMP/ext/lib", "-lext", "-Wl,-rpath,$TMP/ext/lib"]
EOF
printf 'int ext_value();\nint main() { return ext_value() == 1 ? 0 : 1; }\n' > extapp/src/main.cpp
cd extapp
"$MCPP" pack --format dir > ext.log 2>&1 || fail "the pack with a prebuilt library failed" ext.log
etree=$(find target/dist -mindepth 1 -maxdepth 1 -type d -name 'extapp-0.1.0-*' | head -1)
grep -qxF "needs${TAB}libext.so${TAB}lib/libext.so" "$etree.stage-manifest" \
    || fail "libext.so is not stated as bundled" "$etree.stage-manifest"
rm "$TMP/ext/lib/libext.so"
if "$MCPP" pack --format dir > gone.log 2>&1; then
    fail "--format dir succeeded with libext.so gone" gone.log
fi
grep -q "libext.so: the loader finds no file for it" gone.log \
    || fail "the refusal does not name libext.so" gone.log
echo "  ok: dir refuses, naming the library the loader cannot find"

echo "669: an ELF host pack states its closure OK"
