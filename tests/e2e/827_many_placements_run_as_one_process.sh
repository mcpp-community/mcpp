#!/usr/bin/env bash
# requires: unix-shell
# 827_many_placements_run_as_one_process.sh -- #734 E4.
#
# The files a program's runtime needs beside it are placed by one process, not
# one per file: a first Windows build spent 4.5 s on 1270 per-file placements,
# against 0.5 s for one copying process. `mcpp stage --list` reads the
# `<source>\t<destination>` pairs of a list the plan writes, and keeps the
# single-file semantics for each destination.
#
#   S1  50 deploy entries produce one `stage_list` edge and no `stage_file`
#       edge, and every file arrives;
#   S2  a build with nothing changed runs no placement;
#   S3  changing one source rewrites its destination only: every other
#       destination keeps its time stamp;
#   S4  one deploy entry keeps the per-file edge, byte for byte.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

mkdir -p p/src p/data
cd p
for i in $(seq 1 50); do echo "$i" > data/f$i.txt; done
printf 'int main() { return 0; }\n' > src/main.cpp
printf '[package]\nname = "place827"\nversion = "0.1.0"\n' > mcpp.toml
program() {  # $1 = how many entries
    cat > build.mcpp <<EOF
import std;
import mcpp.core;
int main() {
    for (int i = 1; i <= $1; ++i) {
        const std::string f = "data/f" + std::to_string(i) + ".txt";
        mcpp::deploy(f.c_str(), "data");
    }
    return 0;
}
EOF
}

# S1
program 50
"$MCPP" build > s1.log 2>&1 || fail "S1: the build failed" s1.log
NINJA=$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)
[ "$(grep -c ': stage_list ' "$NINJA")" = 1 ] || fail "S1: not exactly one stage_list edge" "$NINJA"
grep -q ': stage_file .*data' "$NINJA" && fail "S1: a per-file placement edge remains" "$NINJA"
OUT=$(dirname "$NINJA")
[ "$(ls "$OUT"/bin/data 2>/dev/null | wc -l)" -eq 50 ] || [ "$(find "$OUT" -path '*data/f*.txt' | wc -l)" -ge 50 ] \
    || fail "S1: not every file was placed" s1.log
DEST1=$(find "$OUT" -path '*/data/f1.txt' | head -1)
DEST2=$(find "$OUT" -path '*/data/f2.txt' | head -1)
[ -n "$DEST1" ] && [ -n "$DEST2" ] || fail "S1: the placed files are not found under $OUT" s1.log

# S2
before=$(grep -c "placements\|data/f" "$OUT/.ninja_log" || true)
touch src/main.cpp
"$MCPP" build > s2.log 2>&1 || fail "S2: the build failed" s2.log
after=$(grep -c "placements\|data/f" "$OUT/.ninja_log" || true)
[ "$before" = "$after" ] || fail "S2: a build with no placement change ran the placement edge" "$OUT/.ninja_log"

# S3
touch -r "$DEST2" s3.ref   # a reference file, since the stat options differ between GNU and BSD
sleep 1.1
echo changed > data/f1.txt
touch src/main.cpp
"$MCPP" build > s3.log 2>&1 || fail "S3: the build failed" s3.log
grep -qx changed "$DEST1" || fail "S3: the changed source did not arrive" "$DEST1"
[ -z "$(find "$DEST2" -newer s3.ref)" ] || fail "S3: an unchanged destination was rewritten" s3.log

# S4
program 1
"$MCPP" build > s4.log 2>&1 || fail "S4: the build failed" s4.log
NINJA=$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)
grep -q ': stage_list ' "$NINJA" && fail "S4: one entry produced a list edge" "$NINJA"
grep -q ': stage_file .*f1.txt' "$NINJA" || fail "S4: the single entry lost its per-file edge" "$NINJA"

echo "OK"
