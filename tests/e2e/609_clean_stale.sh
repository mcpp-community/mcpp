#!/usr/bin/env bash
# requires:
# 609_clean_stale.sh — `mcpp clean --stale` (#565): under target/<triple>/,
# only fingerprint directories that no recorded build considers current are
# removed. A dev and a release build are both current (two fingerprints, two
# entries in target/.build_cache); a directory nobody recorded is stale.
# --dry-run lists and deletes nothing; a plain `mcpp clean --stale` deletes
# exactly the old stale one, keeps a freshly written unrecorded one (a
# `mcpp test` build looks like that) unless --older-than 0, and the survivors
# are not rebuilt afterwards.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new stale > /dev/null
cd stale

# Refuses before any build: there is no record of what is current.
rc=0
out=$("$MCPP" clean --stale 2>&1) || rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL: --stale before any build should refuse: $out"; exit 1; }
echo "$out" | grep -q 'build_cache' || { echo "FAIL: refusal should name the record: $out"; exit 1; }

"$MCPP" build > /dev/null
"$MCPP" build --release > /dev/null

triple=$(ls target | grep -v '^\.' | head -1)
[[ -n "$triple" ]] || { echo "FAIL: no target/<triple>/ after build"; exit 1; }
before=$(ls "target/$triple" | wc -l)
[[ "$before" -eq 2 ]] || { echo "FAIL: expected 2 fingerprint dirs (dev+release), got $before: $(ls target/$triple)"; exit 1; }

# A fingerprint directory nobody recorded, from long ago.
mkdir -p "target/$triple/deadbeefdeadbeef/bin"
echo stale > "target/$triple/deadbeefdeadbeef/bin/leftover"
touch -t 200001010000 "target/$triple/deadbeefdeadbeef" "target/$triple/deadbeefdeadbeef/bin/leftover"

# An unrecorded directory written just now (what a `mcpp test` build looks
# like to the record): kept by the default --older-than 1d.
mkdir -p "target/$triple/cafef00dcafef00d"
echo latest > "target/$triple/cafef00dcafef00d/build.ninja"

# --dry-run: names the old one, keeps the newest, deletes nothing.
out=$("$MCPP" clean --stale --dry-run 2>&1)
echo "$out" | grep -q 'would remove target/.*/deadbeefdeadbeef' || { echo "FAIL: dry-run did not list the stale dir: $out"; exit 1; }
echo "$out" | grep -q 'kept .*cafef00dcafef00d' || { echo "FAIL: dry-run should keep the freshly written unrecorded dir: $out"; exit 1; }
[[ -d "target/$triple/deadbeefdeadbeef" ]] || { echo "FAIL: dry-run deleted something"; exit 1; }
[[ $(ls "target/$triple" | wc -l) -eq 4 ]] || { echo "FAIL: dry-run changed target/"; exit 1; }

# The real thing: exactly the old unrecorded one goes.
out=$("$MCPP" clean --stale 2>&1)
echo "$out" | grep -q 'removed target/.*/deadbeefdeadbeef' || { echo "FAIL: clean --stale did not report the removed dir: $out"; exit 1; }
[[ ! -d "target/$triple/deadbeefdeadbeef" ]] || { echo "FAIL: stale dir survived"; exit 1; }
[[ -d "target/$triple/cafef00dcafef00d" ]] || { echo "FAIL: fresh unrecorded dir was removed"; exit 1; }
[[ $(ls "target/$triple" | wc -l) -eq 3 ]] || { echo "FAIL: a current dir was removed: $(ls target/$triple)"; exit 1; }

# --older-than 0 keeps nothing unrecorded; a bad duration is refused.
rc=0; out=$("$MCPP" clean --stale --older-than nonsense 2>&1) || rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL: bad --older-than should be refused: $out"; exit 1; }
out=$("$MCPP" clean --stale --older-than 0 2>&1)
[[ ! -d "target/$triple/cafef00dcafef00d" ]] || { echo "FAIL: --older-than 0 kept the fresh unrecorded dir: $out"; exit 1; }
[[ $(ls "target/$triple" | wc -l) -eq 2 ]] || { echo "FAIL: --older-than 0 removed a current dir: $(ls target/$triple)"; exit 1; }

# Nothing stale left: says so, changes nothing.
out=$("$MCPP" clean --stale 2>&1)
echo "$out" | grep -q 'Nothing stale' || { echo "FAIL: second pass should report nothing stale: $out"; exit 1; }
[[ $(ls "target/$triple" | wc -l) -eq 2 ]] || { echo "FAIL: second pass removed a current dir"; exit 1; }

# --dry-run alone implies --stale (lists, deletes nothing); --stale with
# --bmi-cache is refused, since the global build cache has its own gc.
mkdir -p "target/$triple/feedfacefeedface"
touch -t 200001010000 "target/$triple/feedfacefeedface"
out=$("$MCPP" clean --dry-run 2>&1)
echo "$out" | grep -q 'feedfacefeedface' || { echo "FAIL: --dry-run alone did not list the stale dir: $out"; exit 1; }
[[ -d "target/$triple/feedfacefeedface" ]] || { echo "FAIL: --dry-run alone deleted"; exit 1; }
rc=0
out=$("$MCPP" clean --stale --bmi-cache 2>&1) || rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL: --stale --bmi-cache should be refused: $out"; exit 1; }
[[ -d "target/$triple/feedfacefeedface" ]] || { echo "FAIL: refused combination still deleted"; exit 1; }
"$MCPP" clean --stale > /dev/null

# Survivors are intact: both profiles rebuild without relinking anything.
before=$(stat -c '%n %Y' target/"$triple"/*/bin/stale | sort)
"$MCPP" build > /dev/null
"$MCPP" build --release > /dev/null
after=$(stat -c '%n %Y' target/"$triple"/*/bin/stale | sort)
[[ "$before" == "$after" ]] || { echo "FAIL: a current directory was rebuilt after clean --stale"; echo "$before"; echo "$after"; exit 1; }

echo "OK"
