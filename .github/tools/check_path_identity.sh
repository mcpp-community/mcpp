#!/usr/bin/env bash
# The resolutions of a path to its canonical form only fall (design 2026-11-27 §4.6, mcpp#1024).
#
# A path the user or the environment gives is opened and handed to a program as it is spelled; its
# canonical form is an identity, asked of mcpp.libs.path_identity, which reports when it cannot be
# read. Each file's count of `canonical(` and `weakly_canonical(` called on std::filesystem is at
# most its line in path-identity.baseline; a file not listed has none. A line falls as its calls
# move to the identity module (an identity) or to the given spelling (an address).
set -u
cd "$(dirname "$0")/../.."

baseline=.github/tools/path-identity.baseline
[ -f "$baseline" ] || { echo "::error::missing $baseline"; exit 1; }

current=$(grep -rcE '(filesystem|fs)::(weakly_)?canonical\(' src modules \
              --include='*.cpp' --include='*.cppm' 2>/dev/null \
          | grep -v ':0$' | grep -v '^modules/libs/src/path_identity.cppm:' | grep -v '/tests/' \
          | sed 's/:/ /' | sort)

rc=0
while read -r file n; do
    [ -n "$file" ] || continue
    allowed=$(awk -v f="$file" '$1 == f { print $2 }' "$baseline")
    if [ -z "$allowed" ]; then
        echo "::error::$file resolves a path to its canonical form $n time(s); ask mcpp.libs.path_identity, or open the given spelling"
        rc=1
    elif [ "$n" -gt "$allowed" ]; then
        echo "::error::$file resolves a path to its canonical form $n times, more than the $allowed of $baseline"
        rc=1
    elif [ "$n" -lt "$allowed" ]; then
        echo "note: $file resolves $n times, fewer than the $allowed of $baseline; lower its line"
    fi
done <<< "$current"
[ "$rc" = 0 ] && echo "OK: no file resolves a path to its canonical form more often than $baseline states"
exit "$rc"
