#!/usr/bin/env bash
# requires: gcc
# Incremental x generated_files: an unchanged [generated_files] entry must NOT
# be rewritten when prepare_build runs — ninja is mtime-driven, so a gratuitous
# rewrite of a materialized header recompiles every TU that #includes it (via
# depfiles). A truly no-op rebuild is masked by the P0 fast-path (prepare is
# skipped), so the load-bearing case is: touch an UNRELATED source, which
# abandons the fast-path and re-runs prepare (and materialization). Assert the
# generated header keeps its mtime and the including TU is not recompiled;
# a content change must still rewrite and rebuild (no false-fresh).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1"; }

cd "$TMP"
"$MCPP" new genincr > /dev/null
cd genincr
rm -f src/main.cpp

cat > mcpp.toml <<'EOF'
[package]
name    = "genincr"
version = "0.1.0"

[generated_files]
"src/gen/config.h" = """
#define GEN_ANSWER 41
"""
EOF

cat > src/main.cpp <<'EOF'
#include "gen/config.h"
#include <cstdio>
extern int other();
int main() {
    std::printf("gen = %d\n", GEN_ANSWER + other());
    return 0;
}
EOF

cat > src/other.cpp <<'EOF'
int other() { return 0; }
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
[[ -f src/gen/config.h ]] || { echo "generated file not materialized"; exit 1; }

main_obj=$(find target -name 'main*.o' | head -1)
other_obj=$(find target -name 'other*.o' | head -1)
[[ -n "$main_obj" && -n "$other_obj" ]] || { echo "object files not found under target/"; exit 1; }

gen_before=$(mtime src/gen/config.h)
main_before=$(mtime "$main_obj")

sleep 1 # make a rewrite observable at 1s mtime granularity

# Touch an unrelated TU: the fast-path is abandoned, prepare_build re-runs,
# and generated_files are re-materialized.
touch src/other.cpp
# (c) the incremental rebuild still succeeds ...
"$MCPP" build > rebuild.log 2>&1 || { cat rebuild.log; echo "incremental rebuild failed"; exit 1; }
# ... and other.o really was recompiled (prepare + compile did run).
other_after=$(mtime "$other_obj")
[[ "$other_after" -gt "$gen_before" ]] || { echo "other.o was not recompiled — test premise broken"; exit 1; }
# (a) the generated header was NOT rewritten (mtime preserved) ...
gen_after=$(mtime src/gen/config.h)
[[ "$gen_before" == "$gen_after" ]] || { echo "generated file rewritten on incremental rebuild ($gen_before -> $gen_after)"; exit 1; }
# (b) ... so main.o (which #includes it, untouched) was NOT recompiled.
main_after=$(mtime "$main_obj")
[[ "$main_before" == "$main_after" ]] || { echo "main.o recompiled on incremental rebuild ($main_before -> $main_after)"; exit 1; }

# Content change: the file must be rewritten and the TU recompiled (no
# false-fresh — change detection lives in the fingerprint).
sed -i.bak 's/GEN_ANSWER 41/GEN_ANSWER 42/' mcpp.toml
sleep 1
"$MCPP" build > change.log 2>&1 || { cat change.log; echo "rebuild after content change failed"; exit 1; }
gen_changed=$(mtime src/gen/config.h)
[[ "$gen_after" != "$gen_changed" ]] || { echo "generated file not rewritten after content change"; exit 1; }
grep -q "GEN_ANSWER 42" src/gen/config.h || { echo "generated file has stale content"; exit 1; }
# The content change alters the fingerprint, which may relocate the build
# dir — assert on the NEWEST main*.o, wherever it lives.
main_changed=$(find target -name 'main*.o' | while read -r f; do mtime "$f"; done | sort -n | tail -1)
[[ "$main_changed" -ge "$gen_changed" ]] || { echo "main.o not recompiled after content change"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "gen = 42" ]] || { echo "stale binary after content change: $out"; exit 1; }

echo "OK"
