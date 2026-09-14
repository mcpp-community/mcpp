#!/usr/bin/env bash
# requires: macos
# 668 -- #634 A3 on the Mach-O row. `mcpp pack` reads a Mach-O program's load
# commands and stages the dylibs it needs beside it in `bin/`, where the
# `@loader_path` rpath a consumer of a graph-built dylib links with finds them:
# no load command is edited and nothing is re-signed. The stage manifest names
# each dylib in a `needs` line, and the OS's own libraries as `platform`.
#
# The criterion is the program running from the staged tree after the build
# tree is gone, and failing with "Library not loaded" once the staged dylib is
# removed -- a tree that ran only while the build directory existed would pass
# every check but that one. Measured on macos-15 before the change (the
# triage record's reading C5): exit 7 staged by hand, exit 134 without the
# dylib, and `mcpp pack --format dir` refused the program.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
TAB=$(printf '\t')

cd "$TMP"
mkdir -p fw/src app/src
cat > fw/mcpp.toml <<'EOF'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
EOF
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > fw/src/fw.cppm

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import fw;
int main() { std::puts("1-2-3"); return fw_anchor() == 41 ? 7 : 1; }
EOF
cd app

# ── 1. --format dir stages the program and its dylib ──────────────────────
"$MCPP" pack --format dir > dir.log 2>&1 || fail "mcpp pack --format dir failed" dir.log
tree=$(find target/dist -mindepth 1 -maxdepth 1 -type d -name 'app-0.1.0-*' | head -1)
[ -n "$tree" ] || fail "no staged tree under target/dist" dir.log
[ -x "$tree/bin/app" ]         || fail "the staged tree has no bin/app" dir.log
[ -f "$tree/bin/libfw.dylib" ] || fail "the staged tree has no bin/libfw.dylib beside the program" dir.log

manifest="$tree.stage-manifest"
[ "$(sed -n '1p' "$manifest")" = "closure = walked" ] \
    || fail "the manifest's first line is not 'closure = walked'" "$manifest"
grep -qxF "needs${TAB}@rpath/libfw.dylib${TAB}bin/libfw.dylib" "$manifest" \
    || fail "the manifest does not state the dylib beside the program" "$manifest"
grep -q "^needs${TAB}/usr/lib/libSystem.B.dylib${TAB}platform\$" "$manifest" \
    || fail "the manifest does not state libSystem as the platform's" "$manifest"
echo "  ok: bin/libfw.dylib is staged and the manifest names both kinds"

# ── 2. the staged program runs with the build tree gone ───────────────────
mv target "$TMP/moved-target"
dest="$TMP/installed"
mkdir -p "$dest"
cp -R "$TMP/moved-target/dist/$(basename "$tree")" "$dest/"
staged="$dest/$(basename "$tree")"
rm -rf "$TMP/moved-target"

set +e
"$staged/bin/app" > run.log 2>&1; rc=$?
set -e
[ "$rc" -eq 7 ] || fail "the staged program exited $rc, not 7" run.log
grep -q '^1-2-3$' run.log || fail "the staged program did not print 1-2-3" run.log
echo "  ok: the staged program runs with the build tree deleted (exit 7)"

# ── 3. and it is the staged dylib that loaded ─────────────────────────────
rm "$staged/bin/libfw.dylib"
set +e
"$staged/bin/app" > gone.log 2>&1; rc=$?
set -e
[ "$rc" -ne 0 ] && [ "$rc" -ne 7 ] || fail "the program still ran without its staged dylib (exit $rc)" gone.log
grep -q "Library not loaded" gone.log || fail "the failure is not a missing library" gone.log
echo "  ok: without the staged dylib the program stops with Library not loaded"

# ── 4. --format tar carries the same two files ────────────────────────────
cd "$TMP/app"
"$MCPP" pack > tar.log 2>&1 || fail "mcpp pack (tar) failed" tar.log
archive=$(find target/dist -maxdepth 1 -name 'app-0.1.0-*.tar.gz' | head -1)
[ -n "$archive" ] || fail "no archive under target/dist" tar.log
listing=$(tar -tzf "$archive")
grep -q '/bin/app$' <<<"$listing"         || fail "the archive has no bin/app" tar.log
grep -q '/bin/libfw.dylib$' <<<"$listing" || fail "the archive has no bin/libfw.dylib" tar.log
echo "  ok: the archive carries bin/app and bin/libfw.dylib"

echo "668: a Mach-O program is staged with its closure OK"
