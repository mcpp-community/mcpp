#!/usr/bin/env bash
# requires: gcc
# 1000 -- a configuration keeps one build directory over time (design
# 2026-10-31 §8). A new fingerprint of a lineage -- the compiler family, the
# target, the standard library's family, the standard and the configuration's
# flags -- takes over the directory of its predecessor, so an mcpp upgrade
# rebuilds the edges whose commands changed in place instead of opening a full
# copy; a predecessor of another toolchain is removed; and the build directories
# no configuration used for `[build] keep_days` are swept after a build.
#
# Criteria:
#   A. A predecessor of the lineage is taken over: the build continues in it
#      and compiles nothing again.
#   B. A predecessor of another toolchain is removed ("Replaced").
#   C. A directory of another lineage is kept.
#   D. The sweep after a build removes an unrecorded directory written longer
#      ago than `keep_days`; `keep_days = 0` (`mcpp self config --keep-days 0`)
#      keeps it.
#   E. The configuration names the build-cache entries it used (`uses.list`),
#      and a build that replays its graph without planning still marks them
#      used: an entry last used 40 days ago is used today after the sweep.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p app/src && cd app
printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[dependencies]\n"compat.zlib" = "1.3.2"\n' > mcpp.toml
printf '#include <zlib.h>\nint main() { return zlibVersion()[0] == 0; }\n' > src/main.cpp
"$MCPP" build > b0.log 2>&1 || fail "the first build failed" b0.log
dir=$(ls -d target/*/*/ | grep -E '/[0-9a-f]{16}/$' | head -1); dir=${dir%/}
triple=$(dirname "$dir")
[ -f "$dir/lineage" ] && [ -f "$dir/toolchain" ] || fail "the directory records no lineage" <(ls -a "$dir")
compiles() { grep -c 'main' "$1/.ninja_log"; }
before=$(compiles "$dir")

# A
mv "$dir" "$triple/0000000000000000"
"$MCPP" build > a.log 2>&1 || fail "the build after the move failed" a.log
[ -d "$dir" ] && [ ! -d "$triple/0000000000000000" ] || fail "the predecessor was not taken over" <(ls "$triple")
[ "$(compiles "$dir")" = "$before" ] || fail "taking the predecessor over compiled again" "$dir/.ninja_log"

# B
mv "$dir" "$triple/0000000000000000"
printf 'compiler another\n' > "$triple/0000000000000000/toolchain"
"$MCPP" build > b.log 2>&1 || fail "the build after a toolchain change failed" b.log
[ ! -d "$triple/0000000000000000" ] || fail "a predecessor of another toolchain was kept" <(ls "$triple")
grep -q 'Replaced' b.log || fail "the replacement was not said" b.log

# C
mkdir -p "$triple/1111111111111111"
printf 'compiler other\n' > "$triple/1111111111111111/lineage"
touch "$triple/1111111111111111/build.ninja"
"$MCPP" build > c.log 2>&1 || fail "the build beside another lineage failed" c.log
[ -d "$triple/1111111111111111" ] || fail "a directory of another lineage was taken" <(ls "$triple")

# D
old="$triple/2222222222222222"
mkdir -p "$old" && touch "$old/build.ninja"
touch -d '5 days ago' "$old/build.ninja" "$old"
"$MCPP" self config --keep-days 0 > dc.log 2>&1 || fail "self config --keep-days 0 failed" dc.log
rm -f target/.mcpp-swept
"$MCPP" build > d0.log 2>&1 || fail "the build with keep_days = 0 failed" d0.log
[ -d "$old" ] || fail "keep_days = 0 swept a directory" d0.log
"$MCPP" self config --keep-days 3 > dc.log 2>&1 || fail "self config --keep-days 3 failed" dc.log
rm -f target/.mcpp-swept
"$MCPP" build > d1.log 2>&1 || fail "the build that sweeps failed" d1.log
[ ! -d "$old" ] || fail "a directory no configuration used for 5 days was kept" d1.log <(ls "$triple")
grep -q 'Swept' d1.log || fail "the sweep was not said" d1.log
[ -d "$dir" ] || fail "the sweep removed the configuration in use" <(ls "$triple")

# E
record=$(sed -n 's/^cache \(.*compat.zlib.*\)$/\1/p' "$dir/uses.list" | head -1)
[ -n "$record" ] && [ -f "$record" ] || fail "the configuration names no cache entry it used" "$dir/uses.list"
touch -d '40 days ago' "$record"
"$MCPP" build > e0.log 2>&1 || fail "the replayed build failed" e0.log
rm -f target/.mcpp-swept
"$MCPP" build > e1.log 2>&1 || fail "the build that sweeps failed" e1.log
[ -n "$(find "$record" -mtime -1)" ] || fail "a replayed build did not mark its cache entry used" <(stat "$record")

echo "OK"
