#!/usr/bin/env bash
# The waits for time in the tests only fall (design 2026-11-20 §5.20).
#
# A test waits for an event the program under test gives -- a line of output, a
# file, its exit -- and not for a time: a sleep long enough on one machine is
# short on a loaded one, and a poll hides how long the event took. Each file's
# count of `sleep_for`, `sleep_until` and shell `sleep <n>` is at most its line
# in waits.baseline; a file not listed has none. A wait whose time is itself
# what the case asserts says so in a comment on the line above it, and stays.
set -u
cd "$(dirname "$0")/../.."

baseline=.github/tools/waits.baseline
[ -f "$baseline" ] || { echo "::error::missing $baseline"; exit 1; }

current=$(grep -rcE 'sleep_for|sleep_until|(^|[;&|[:space:]])sleep [0-9]' tests \
              --include='*.cpp' --include='*.cppm' --include='*.sh' 2>/dev/null \
          | grep -v ':0$' | sed 's/:/ /' | sort)

rc=0
while read -r file n; do
    [ -n "$file" ] || continue
    allowed=$(awk -v f="$file" '$1 == f { print $2 }' "$baseline")
    if [ -z "$allowed" ]; then
        echo "::error::$file waits for a time $n time(s); a test waits for an event (design 2026-11-20 §5.20)"
        rc=1
    elif [ "$n" -gt "$allowed" ]; then
        echo "::error::$file waits for a time $n times, more than the $allowed of $baseline"
        rc=1
    elif [ "$n" -lt "$allowed" ]; then
        echo "note: $file waits $n times, fewer than the $allowed of $baseline; lower its line"
    fi
done <<< "$current"
[ "$rc" = 0 ] && echo "OK: no test waits for a time more often than $baseline states"
exit "$rc"
