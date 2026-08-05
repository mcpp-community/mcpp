#!/usr/bin/env bash
# 191_link_scale.sh — a link edge with more objects than a command line can hold.
#
# WHAT THIS COVERS THAT NOTHING ELSE DID
#
# mcpp#346: no CI job builds a package of the opencv/ffmpeg magnitude. Every
# job builds either mcpp itself (tens of TUs) or a synthetic e2e project
# (single digits). Link-line length, the response-file path, and ninja graph
# size over a large object set therefore had ZERO coverage, and the whole
# command-length defect family surfaced in the ecosystem rather than in CI:
#
#     #274  ninja goals argv 50781 chars vs cmd.exe 8191
#     #247  Windows CreateProcess 32 KiB
#     #345  POSIX MAX_ARG_STRLEN 128 KiB (ninja spawns `sh -c "<whole cmd>"`)
#     #360  link.exe LNK1170, response-file LINE capped at 128 KiB
#
# 190 asserts the SHAPE of the generated rule (`rspfile_content = $in_newline`,
# 25 objects). This asserts the SCALE: it builds a link edge whose object list
# does not fit in a command line on any supported platform, and requires it to
# link and run. A regression that puts objects back on the command line fails
# here for the same reason opencv-module failed in the ecosystem — except in
# 20 seconds, with no external package, and with a diagnostic that names the
# cause.
#
# WHY THE SIZE ASSERTION IS PART OF THE TEST
#
# The regime is what gives this test its value, and the regime depends on
# incidental things — object naming, the disambiguation prefix, how many files
# the loop below writes. If any of them shrinks the object list back under the
# ceiling, the test would keep passing while covering nothing. So the response
# file's size is asserted directly: below the ceiling the test reports that it
# has stopped covering the axis, rather than passing quietly.
#
# VERIFIED TO FAIL WITHOUT THE FIX
#
# Reverting the generated `cxx_link` rule to its pre-#345 inline form on this
# exact project reproduces the original symptom verbatim:
#
#     ninja: fatal: posix_spawn: Argument list too long
#
# COST
#
# C sources, one trivial function each. Measured at 1.3s wall on a developer
# machine (1400 TUs, parallel). Windows is the expensive leg — per-process
# spawn cost dominates there — and is bounded by the suite's 600s per-test
# timeout.
#
# The names are padded so each object path is ~100 bytes, which is what
# reaches the 128 KiB ceiling at a file count this small: padding trades
# compile time (expensive) for path length (free). Padding is bounded on the
# other side by Windows MAX_PATH — 100 bytes relative, plus the temp directory
# and the build directory, stays near 200 of the 260 available.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── The ceiling this test has to clear ────────────────────────────────────
# POSIX MAX_ARG_STRLEN, the largest of the command-line limits in
# src/build/cmdlimits.cppm — clearing the largest clears all of them, so one
# number works for every platform the suite runs on.
CEILING=$((128 * 1024))

PAD=$(printf 'x%.0s' $(seq 1 88))
N=1400

mkdir -p scale/src
cat > scale/mcpp.toml <<'EOF'
[package]
name    = "scale"
version = "0.1.0"

[build]
c_standard = "c11"
EOF

i=1
while [ "$i" -le "$N" ]; do
    printf 'int f%04d(void) { return %d; }\n' "$i" "$i" > "scale/src/f${i}_${PAD}.c"
    i=$((i + 1))
done
# The bin target is inferred from src/main.cpp; the objects it links are the C
# TUs above. Referencing the FIRST and the LAST of them is what makes a
# truncated object list a link error rather than a smaller binary: a response
# file cut short at any point drops one of these two.
cat > scale/src/main.cpp <<EOF
extern "C" int f0001(void);
extern "C" int f$(printf '%04d' "$N")(void);
int main() { return (f0001() == 1 && f$(printf '%04d' "$N")() == $N) ? 0 : 1; }
EOF

cd scale
"$MCPP" build > b.log 2>&1 || { tail -40 b.log; echo "FAIL: build $N objects"; exit 1; }
echo "  ok: built $((N + 1)) objects"

ninja_file=$(find target -name build.ninja | head -1)
[ -n "$ninja_file" ] || { echo "FAIL: no build.ninja"; exit 1; }
bdir=$(dirname "$ninja_file")
bin_rel=$(cd "$bdir" && ls bin/ 2>/dev/null | head -1)
[ -n "$bin_rel" ] || { echo "FAIL: nothing was linked"; exit 1; }

# Relink with the response file kept, so its real contents can be inspected —
# the same technique as 190, at a size that matters.
(cd "$bdir" && rm -f "bin/$bin_rel" && ninja -d keeprsp "bin/$bin_rel" > relink.log 2>&1) || {
    tail -20 "$bdir/relink.log"; echo "FAIL: relink under -d keeprsp"; exit 1; }

rsp=$(find "$bdir" -name '*.rsp' | head -1)
[ -n "$rsp" ] || { echo "FAIL: -d keeprsp left no response file"; exit 1; }

bytes=$(wc -c < "$rsp" | tr -d ' ')
objects=$(( $(wc -l < "$rsp" | tr -d ' ') + 1 ))

# 1. NON-VACUITY: the object list must not fit in a command line. Without
#    this, everything below could pass on a link edge small enough that the
#    inline form would have worked too.
[ "$bytes" -gt "$CEILING" ] || {
    echo "response file is $bytes bytes over $objects objects; the ceiling is $CEILING"
    echo "FAIL: this test no longer reaches the regime it exists to cover."
    echo "      Raise N or PAD until the object list exceeds the ceiling again."
    exit 1; }
echo "  ok: object list is $bytes bytes ($objects objects), past the ${CEILING}-byte command-line ceiling"

# 2. No single line approaches link.exe's per-line cap (the LNK1170 axis) —
#    asserted here at real scale rather than 190's 25 objects.
longest=$(awk '{ if (length($0) > m) m = length($0) } END { print m+0 }' "$rsp")
[ "$longest" -lt 4096 ] || {
    echo "FAIL: longest response-file line is $longest chars — objects are not one per line"
    exit 1; }
echo "  ok: longest response-file line $longest chars"

# 3. The objects were really consumed. main.cpp calls into both ends of the
#    object list, so a truncated response file cannot reach this point — it
#    fails at link time with an undefined reference. Running the binary closes
#    the remaining gap: that the values arriving at runtime are the ones the
#    two TUs return.
"./$bdir/bin/$bin_rel" || { echo "FAIL: linked binary did not run cleanly"; exit 1; }
echo "  ok: linked binary runs"

echo "OK"
