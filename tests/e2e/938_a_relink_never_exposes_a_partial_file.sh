#!/usr/bin/env bash
# 938 -- a linked artifact is published whole (design 2026-10-26 D22): a
# process that starts the program while it is relinked starts the old file or
# the new one, never half of either.
#
# GNU ld unlinks and writes in place, gold writes in place, and macOS lets a
# running program's file be overwritten: four launchers over 25 relinks saw
# thousands of EACCES / ETXTBSY / ENOEXEC (mcpp#840, E23). The link now writes
# `<out>.tmp` and renames it over the name.
#   A  the link rule writes a temporary name and renames it (POSIX);
#   B  starting the program in a loop while it is relinked never fails to exec.
set -e
case "$(uname -s)" in Linux|Darwin) ;; *) echo "SKIP: POSIX publication"; exit 0 ;; esac
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
printf 'int main() { return 0; }\n' > src/main.cpp
"$MCPP" build > a.log 2>&1 || fail "the build failed" a.log
ninja_file=$(find target -name build.ninja | head -1)
grep -A1 '^rule cxx_link' "$ninja_file" | grep -q '\$out.tmp.*&& mv -f \$out.tmp \$out' \
    || fail "A: the link rule does not publish through a temporary name" "$ninja_file"

exe=$(find target -path '*/bin/app' -type f | head -1)
errors="$TMP/errors"
: > "$errors"
( end=$((SECONDS + 20))
  while [[ $SECONDS -lt $end && ! -e "$TMP/stop" ]]; do
      "$exe" 2>>"$errors" >/dev/null || echo "exec failed: $?" >> "$errors"
  done ) &
for i in $(seq 1 12); do
    printf 'int main() { volatile int x = %d; return x - %d; }\n' "$i" "$i" > src/main.cpp
    "$MCPP" build > b.log 2>&1 || { touch "$TMP/stop"; wait; fail "B: relink $i failed" b.log; }
done
touch "$TMP/stop"
wait
[[ ! -s "$errors" ]] || fail "B: a start met a partially written program" "$errors"

echo "PASS: 938 a relink never exposes a partial file"
