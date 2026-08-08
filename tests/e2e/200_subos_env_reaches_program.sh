#!/usr/bin/env bash
# requires: elf
# 200_subos_env_reaches_program.sh — a subos's declared environment must reach
# the program mcpp launches (mcpp#352).
#
# A program needs three things: it links (bootstrap), it finds its libraries
# (RPATH), and it is told where its runtime data lives (env). mcpp supplied the
# first two and nothing for the third: xlings's graphics packages declare
# LIBGL_DRIVERS_PATH and friends into the subos, `xlings subos use` applied
# them, and `mcpp run` did not. That is why a GLFW binary could link cleanly
# and exit 255 with no output at all.
#
# The probe variable here is deliberately NOT a graphics one. mcpp does not
# know what any of these variables mean -- it carries whatever the subos
# declares -- and a test naming LIBGL_DRIVERS_PATH would quietly suggest
# otherwise.
#
# The JSON below is xlings's REAL shape: `envs` is an object keyed by binding,
# whose values are arrays of declarations. The first version of this test wrote
# an array of {binding, decls} -- a shape xlings never produces -- and it
# passed, because the reader had been written from the same misunderstanding.
# Do not "simplify" this structure; it is a wire format, not a convenience.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A subos that declares one variable, in xlings's own schema. Built here rather
# than by mutating the developer's real subos: an earlier e2e wrote through a
# symlink and permanently broke a real toolchain, and MCPP_SUBOS_DIR exists so
# this test never has to go near one.
subos="$TMP/subos"
mkdir -p "$subos/usr/lib/dri"
cat > "$subos/.xlings.json" <<'EOF'
{ "workspace": {},
  "subos_info": { "schema_version": 1, "runtime": "glibc@2.39",
    "envs": { "probe@1": [
      { "var": "MCPP_E2E_PROBE", "op": "prepend",
        "value": "${subosdir}/usr/lib/dri" } ] } } }
EOF

cd "$TMP"
"$MCPP" new hello > /dev/null
cd hello
cat > src/main.cpp <<'EOF'
#include <cstdio>
#include <cstdlib>
int main() {
    const char* v = std::getenv("MCPP_E2E_PROBE");
    std::printf("PROBE=%s\n", v ? v : "(unset)");
    return 0;
}
EOF

# 1. `mcpp run` hands it to the program.
out=$(MCPP_SUBOS_DIR="$subos" "$MCPP" run 2>&1) || {
    echo "mcpp run failed:"; echo "$out"; exit 1; }
echo "$out" | grep -q "PROBE=$subos/usr/lib/dri" || {
    echo "the subos's declared environment did not reach the program:"
    echo "$out"
    exit 1
}

# 1b. THE SECOND RUN, which takes the cached fast path.
#
# This is the assertion that matters most, and the one a single-run test
# cannot make. The fast path builds its own child environment and skips
# prepare_build entirely; when it was first written it did not know about
# subos declarations at all, so a program worked on the run right after a
# build and silently stopped finding its runtime data on every run after
# that. For a GL application that is "it worked once and now the window is
# black", with nothing in between to attribute it to.
out_cached=$(MCPP_SUBOS_DIR="$subos" "$MCPP" run 2>&1) || {
    echo "cached mcpp run failed:"; echo "$out_cached"; exit 1; }
# Self-check FIRST: prove this run actually took the fast path, otherwise the
# assertion below is vacuous and would keep passing after the coverage it
# exists for has silently gone away. The full path resolves the toolchain and
# says so; the fast path skips prepare_build entirely and never prints it.
echo "$out_cached" | grep -q 'Resolving toolchain' && {
    echo "the second run did NOT take the cached fast path, so this test is"
    echo "  not covering it. Fix the test before trusting the assertion below:"
    echo "$out_cached"
    exit 1
}
echo "$out_cached" | grep -q "PROBE=$subos/usr/lib/dri" || {
    echo "the cached fast path dropped the subos environment — the program"
    echo "  gets a different environment on its second run than its first:"
    echo "$out_cached"
    exit 1
}

# 1c. ...and the environment is re-READ, not cached with the build. A user
#     who installs a graphics stack between two runs must get it without
#     rebuilding, so changing the declaration must change the next run.
sed -i 's#/usr/lib/dri#/usr/lib/dri2#' "$subos/.xlings.json"
out_changed=$(MCPP_SUBOS_DIR="$subos" "$MCPP" run 2>&1) || {
    echo "run after changing the declaration failed:"; echo "$out_changed"; exit 1; }
echo "$out_changed" | grep -q "PROBE=$subos/usr/lib/dri2" || {
    echo "the subos declaration changed but the program still sees the old"
    echo "  value — the environment was cached with the build instead of"
    echo "  being read from the subos:"
    echo "$out_changed"
    exit 1
}
sed -i 's#/usr/lib/dri2#/usr/lib/dri#' "$subos/.xlings.json"

# 2. Without a subos saying anything, nothing is invented.
out2=$("$MCPP" run 2>&1) || { echo "plain mcpp run failed:"; echo "$out2"; exit 1; }
echo "$out2" | grep -q 'PROBE=(unset)' || {
    echo "a variable appeared with no subos declaring it:"
    echo "$out2"
    exit 1
}

# 2b. A cache written before this mcpp knew about subos environments must NOT
#     be replayed by the fast path. Simulated by stripping the field, which is
#     exactly what an older mcpp's cache looks like.
#
#     Without this the fix survives an upgrade in name only: the fast path's
#     identity is the profile, the cache mode and the resource list, and its
#     fingerprint check compares a cached entry against itself -- so nothing
#     notices that a different mcpp wrote it, and an upgraded mcpp would keep
#     running the pre-upgrade build with no subos environment at all.
cache="$TMP/hello/target/.build_cache"
[ -f "$cache" ] || { echo "no build cache to age"; exit 1; }
grep -q '^subos=' "$cache" || { echo "cache has no subos= line to strip"; exit 1; }
grep -v '^subos=' "$cache" > "$cache.old" && mv "$cache.old" "$cache"
aged=$(MCPP_SUBOS_DIR="$subos" "$MCPP" run 2>&1) || {
    echo "run against an aged cache failed:"; echo "$aged"; exit 1; }
echo "$aged" | grep -q 'Resolving toolchain' || {
    echo "an aged cache was replayed by the fast path — the subos environment"
    echo "  would be missing for every run after an upgrade:"
    echo "$aged"
    exit 1
}
echo "$aged" | grep -q "PROBE=$subos/usr/lib/dri" || {
    echo "the rebuild after an aged cache did not apply the environment:"
    echo "$aged"; exit 1; }
grep -q '^subos=' "$cache" || {
    echo "the rebuild did not record subos= , so every later run repeats it"; exit 1; }

# 3. A subos with no self-description degrades quietly and still runs. This is
#    the state of every subos created before xlings grew the block, so it must
#    not be an error.
bare="$TMP/bare"
mkdir -p "$bare"
echo '{ "workspace": {} }' > "$bare/.xlings.json"
out3=$(MCPP_SUBOS_DIR="$bare" "$MCPP" run 2>&1) || {
    echo "a subos without subos_info broke the run:"; echo "$out3"; exit 1; }
echo "$out3" | grep -q 'PROBE=(unset)' || {
    echo "unexpected output from a subos with no declarations:"; echo "$out3"; exit 1; }

echo "PASS: subos environment reaches the program, and nothing is invented"
