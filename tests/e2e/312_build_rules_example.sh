#!/usr/bin/env bash
# requires: gcc
# 312_build_rules_example.sh — examples/08-build-rules actually builds and runs.
#
# mcpp's `examples/` are not otherwise exercised by CI, so an example that stops
# working says nothing until a reader tries it. This one is worth the e2e
# because it is the only place the ecosystem-facing shape of a rule package is
# written down as running code: two rule packages, one depending on the other
# through its own `[build-dependencies]`, covering `role = "check"` and a
# generated source, and a consumer that uses both.
#
# It is copied to a temporary directory rather than built in place: building in
# the worktree would leave target/ directories inside the repository and would
# make a second run measure the first one's cache.
set -e

SRC="$(cd "$(dirname "$0")/../.." && pwd)/examples/08-build-rules"
[[ -d "$SRC" ]] || { echo "FAIL: $SRC is missing"; exit 1; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cp -r "$SRC" "$TMP/ex"
# Anything a previous run left behind travels with the copy, and the object
# count below would then be measuring that run instead of this one. Caught by
# the denominator on the first attempt: four objects where two were expected.
find "$TMP/ex" -maxdepth 3 -type d -name target -exec rm -rf {} + 2>/dev/null || true
find "$TMP/ex" -maxdepth 3 -name mcpp.lock -delete 2>/dev/null || true
cd "$TMP/ex/app"

"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: the example did not build"; exit 1; }

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "hello from an embedded asset" ]] || {
    echo "FAIL: the embedded asset did not reach the program: '$out'"; exit 1; }

# The rules are build-time only. Asserted on the ARTIFACT: a rule's objects have
# no business in the consumer's binary, and `Compiling rules-embed (path)` in
# the log is the host-module compile, not evidence either way.
mapfile -t objs < <(find target -path '*obj*' -name '*.o' -printf '%f\n' | sort)
for o in "${objs[@]}"; do
    case "$o" in
        main.o|embed_greeting.o) ;;
        *) printf '  %s\n' "${objs[@]}"
           echo "FAIL: unexpected object '$o' — a rule package reached the link"
           exit 1 ;;
    esac
done
# And the denominator: an empty object list would pass the loop above while
# proving nothing.
(( ${#objs[@]} == 2 )) || {
    printf '  %s\n' "${objs[@]}"
    echo "FAIL: expected exactly 2 objects, found ${#objs[@]}"; exit 1; }

# The generated source is a graph node, so editing its INPUT re-runs that edge
# and changes the program. This is what `role`-based actions buy over writing
# the file eagerly from build.mcpp.
sleep 1
printf 'a different embedded asset\n' > assets/greeting.txt
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: rebuild failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "a different embedded asset" ]] || {
    echo "FAIL: editing the asset did not re-run the embed action: '$out'"; exit 1; }

# The check's ANALYSER never touches the stamp — `tools/check.sh` reads a file
# and answers with an exit code, which is what clang-tidy does. mcpp creates
# the stamp when the command succeeds.
#
# ⚠️ ASSERT ON THE STAMP AND ON THE RE-RUN, NOT ON THE BUILD SUCCEEDING.
# Measured with the wrapper removed: ninja does NOT fail when a declared output
# goes unproduced. It leaves the file missing and re-runs that edge on every
# build, forever — so the build stays green, this script's earlier assertions
# stay green, and the only visible symptom is work being redone. A test that
# checked exit codes here would have passed either way.
mapfile -t stamps < <(find target -name '*.stamp')
(( ${#stamps[@]} == 1 )) || {
    printf '  %s\n' "${stamps[@]}"
    echo "FAIL: expected exactly one check stamp, found ${#stamps[@]}"; exit 1; }

"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: no-op rebuild failed"; exit 1; }
if grep -q 'CHECK' b3.log; then
    cat b3.log
    echo "FAIL: the check re-ran with nothing changed — its stamp is not satisfying the edge"
    exit 1
fi

# A failing check fails the build. Without this the check role would be
# decoration: it runs beside the compile, so nothing else would notice it.
printf 'int main() { goto done; done: return 0; }\n' > src/main.cpp
if "$MCPP" build > b3.log 2>&1; then
    cat b3.log; echo "FAIL: a failing check did not fail the build"; exit 1
fi
grep -qF "goto" b3.log || {
    cat b3.log; echo "FAIL: the build failed without reporting what the check found"; exit 1; }

echo "OK"
