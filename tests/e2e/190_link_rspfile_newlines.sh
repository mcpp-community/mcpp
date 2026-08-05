#!/usr/bin/env bash
# 190_link_rspfile_newlines.sh — the link response file separates objects by
# NEWLINES, not spaces.
#
# Two ceilings sit between a link edge and the OS, and routing the objects
# through a response file only removes the first:
#
#   1. the COMMAND LINE — Windows CreateProcess 32 KiB, POSIX MAX_ARG_STRLEN
#      128 KiB (ninja spawns `sh -c "<whole command>"`, so the command is one
#      argv entry). Removed by using @rspfile at all — mcpp#344 / PR#345.
#   2. the response file's LINE LENGTH — link.exe caps it at 128 KiB:
#
#          fatal error LNK1170: line in command file contains 135135
#          or more characters
#
#      which is where mcpp-index's opencv-module landed on windows, with every
#      object written onto a single line.
#
# `rspfile_content = $in_newline` removes the second. After it, no ceiling
# scales with the number of objects.
#
# Asserted structurally (the generated rule) AND observably (the file ninja
# actually writes, kept with -d keeprsp) — the first alone would still pass if
# ninja ever changed what $in_newline expands to.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p multi/src
cat > multi/mcpp.toml <<'EOF'
[package]
name    = "multi"
version = "0.1.0"
EOF
# Enough objects that "one per line" is unambiguous — a single-object link
# would look identical either way.
i=1
while [ "$i" -le 24 ]; do
    printf 'int f%d() { return %d; }\n' "$i" "$i" > "multi/src/f$i.cpp"
    i=$((i + 1))
done
printf 'int main() { return 0; }\n' > multi/src/main.cpp

cd multi
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build"; exit 1; }

ninja_file=$(find target -name build.ninja | head -1)
[ -n "$ninja_file" ] || { echo "FAIL: no build.ninja"; exit 1; }

# 1. Structural: no link rule may write its response file on one line.
if grep -qE '^[[:space:]]*rspfile_content = \$in[[:space:]]*$' "$ninja_file"; then
    grep -nE '^[[:space:]]*rspfile_content' "$ninja_file"
    echo "FAIL: a link rule still writes its response file on ONE line (\$in)"
    exit 1
fi
grep -qE '^[[:space:]]*rspfile_content = \$in_newline[[:space:]]*$' "$ninja_file" || {
    grep -nE '^[[:space:]]*rspfile_content' "$ninja_file"
    echo "FAIL: no link rule uses \$in_newline"; exit 1; }
echo "  ok: link rules declare rspfile_content = \$in_newline"

# 2. Observable: ninja keeps the response file under -d keeprsp, and it holds
#    one object per line rather than all of them on the first.
bdir=$(dirname "$ninja_file")
bin_rel=$(cd "$bdir" && ls bin/ 2>/dev/null | head -1)
[ -n "$bin_rel" ] || { echo "FAIL: no linked binary to inspect"; exit 1; }
(cd "$bdir" && rm -f "bin/$bin_rel" && ninja -d keeprsp "bin/$bin_rel" > /dev/null 2>&1) \
    || { echo "FAIL: relink under -d keeprsp"; exit 1; }

rsp=$(find "$bdir" -name '*.rsp' | head -1)
[ -n "$rsp" ] || { echo "FAIL: -d keeprsp left no response file"; exit 1; }

# 25 objects -> 24 newlines (the last line carries no trailing newline).
lines=$(wc -l < "$rsp")
[ "$lines" -ge 20 ] || {
    echo "response file has $lines newline(s):"; head -c 300 "$rsp"; echo
    echo "FAIL: objects are not one-per-line — the LNK1170 shape is back"
    exit 1; }

# ...and no single line is anywhere near link.exe's 128 KiB cap.
longest=$(awk '{ if (length($0) > m) m = length($0) } END { print m+0 }' "$rsp")
[ "$longest" -lt 4096 ] || {
    echo "FAIL: longest response-file line is $longest chars"; exit 1; }
echo "  ok: $((lines + 1)) objects, longest response-file line $longest chars"

echo "OK"
