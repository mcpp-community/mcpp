#!/usr/bin/env bash
# The target matrix declares no `other`, and no `mismatch`.
#
# `expected.tsv` argues this for `mismatch` in its own header -- writing one
# down declares a defect to be the expectation, and the matrix then goes green
# on it. `other` is the same move one level up: `src/build/refusal.cppm` defines
# it as "a refusal that has not been given a code yet", so a row spelling
# `other` freezes an unnamed branch into the expected table.
#
# It was not hypothetical. Until 2026-09-06 the table carried exactly one such
# row -- `x86_64-windows-msvc x llvm@22.1.8` -- while `Code::StdModulePrecompile`
# already existed with a name and had NO WRITER anywhere in the tree. The gap
# was between the declaration and the call, and nothing pointed at it except a
# line of scan output nobody had to read.
#
# Both directions are checked, because a vocabulary that lists a reason no row
# uses is the same defect seen from the other side: a name with no fact under it.
set -uo pipefail
cd "$(dirname "$0")/../.."
table=tests/matrix/expected.tsv
fail=0

for banned in other mismatch; do
    if rows=$(awk -F'\t' -v b="$banned" '!/^#/ && NF>1 && $NF==b {print NR": "$0}' "$table") \
       && [ -n "$rows" ]; then
        echo "::error::$table declares '$banned':"
        echo "$rows" | sed 's/^/    /'
        case "$banned" in
          other) echo "    Give that refusal branch a code in src/build/refusal.cppm and record it"
                 echo "    at the site that returns, then put the code here." ;;
          mismatch) echo "    A mismatch is a defect, not an expectation. Fix mcpp, or make it refuse"
                 echo "    at the decision with a named reason." ;;
        esac
        fail=1
    fi
done

# Every reason the table uses must be a name `refusal.cppm` actually defines.
# A typo here is silent otherwise: the comparison would demand a string the
# engine can never produce, and the cell would read as a permanent mismatch.
known=$(grep -oE 'return "[a-z-]+";' src/build/refusal.cppm | sed 's/return "//; s/";//' | sort -u)
used=$(awk -F'\t' '!/^#/ && NF>1 {print $NF}' "$table" | sort -u)
for r in $used; do
    grep -Fxq "$r" <<<"$known" || {
        echo "::error::$table uses reason '$r', which src/build/refusal.cppm does not define"
        fail=1
    }
done

[ "$fail" -eq 0 ] && echo "OK: the target matrix names every refusal, and names them from the taxonomy"
exit "$fail"
