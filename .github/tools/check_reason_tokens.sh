#!/usr/bin/env bash
# Every reason token the engine can emit is in docs/50's table, and every row
# of that table is a token the engine can emit.
#
# WHY THIS EXISTS. The table is a MACHINE INTERFACE: mcpp-index's compatibility
# measurement reads `[interface-not-provided]` out of a refusal to tell "this
# graph does not supply what the member asked for" from "the member did not
# build", and that distinction decides a published figure. A token the engine
# emits and the table omits is a promise nobody can rely on; a row naming a
# token no branch emits is one a consumer will wait for forever.
#
# IT EXISTS BECAUSE ADDING THE MISSING ONES BY HAND MISSED SOME. Four tokens
# were added to this table on 2026-09-18 as "the ones it was missing"; a
# later enumeration found four more that had been absent the whole time. A set
# compared by reading is a set compared by sampling.
set -u
cd "$(dirname "$0")/../.."

src=src/build/refusal.cppm
doc=docs/50-machine-output.md
[ -f "$src" ] && [ -f "$doc" ] || { echo "::error::missing $src or $doc"; exit 1; }

# `none` is the sentinel for "no refusal was recorded" and names no branch.
# `other` stays in BOTH sets: it is emitted and it is documented.
emitted=$(grep -oE 'return "[a-z][a-z0-9-]*";' "$src" \
          | sed 's/return "//; s/";//' | grep -vx none | sort -u)

# The table is the contiguous run of `| \`token\` |` rows containing `other`,
# which is its documented catch-all. Anchoring on a row rather than on a
# heading keeps this working when the prose around it is rewritten.
n=$(grep -n '^| `other` |' "$doc" | head -1 | cut -d: -f1)
[ -n "$n" ] || { echo "::error::$doc has no \`other\` row; the table moved"; exit 1; }
start=$n
while [ "$start" -gt 1 ] && sed -n "$((start-1))p" "$doc" | grep -q '^|'; do
    start=$((start-1))
done
# A ROW IS SELECTED BY HAVING A DESCRIPTION, NOT BY LOOKING LIKE A ROW. This
# table's COLUMN HEADER is `| \`reason\` | |` --- a backticked name in the first
# cell, exactly the shape of the rows beneath it --- so a pattern that matches
# "backticked name at the start of a line" reports `reason` as a documented
# token. The second cell is what tells a header from a row, so the pattern
# requires a non-empty one.
documented=$(sed -n "${start},${n}p" "$doc" \
             | grep -oE '^\| `[a-z][a-z0-9-]*` \| [^|]+ \|' \
             | sed 's/^| `//; s/`.*//' | sort -u)

missing=$(comm -23 <(printf '%s\n' "$emitted") <(printf '%s\n' "$documented"))
extra=$(comm -13 <(printf '%s\n' "$emitted") <(printf '%s\n' "$documented"))

rc=0
if [ -n "$missing" ]; then
    echo "::error::these reason tokens are emitted by $src and absent from $doc:"
    printf '        %s\n' $missing
    rc=1
fi
if [ -n "$extra" ]; then
    echo "::error::$doc names these tokens and no branch in $src emits them:"
    printf '        %s\n' $extra
    rc=1
fi
# AND THE 简体中文 MIRROR CARRIES THE SAME SET. A translated page falls behind
# by losing rows, and a structural check that only counts headings cannot see
# it: the token names are identical in both languages, so they compare
# directly even though nothing else on the page does.
zh=docs/zh/50-machine-output.md
if [ -f "$zh" ]; then
    zn=$(grep -n '^| `other` |' "$zh" | head -1 | cut -d: -f1)
    if [ -z "$zn" ]; then
        echo "::error::$zh has no \`other\` row; the mirror's table moved"
        rc=1
    else
        zstart=$zn
        while [ "$zstart" -gt 1 ] && sed -n "$((zstart-1))p" "$zh" | grep -q '^|'; do
            zstart=$((zstart-1))
        done
        zdoc=$(sed -n "${zstart},${zn}p" "$zh" \
               | grep -oE '^\| `[a-z][a-z0-9-]*` \| [^|]+ \|' \
               | sed 's/^| `//; s/`.*//' | sort -u)
        zmiss=$(comm -23 <(printf '%s\n' "$documented") <(printf '%s\n' "$zdoc"))
        zextra=$(comm -13 <(printf '%s\n' "$documented") <(printf '%s\n' "$zdoc"))
        if [ -n "$zmiss" ]; then
            echo "::error::$zh is missing reason tokens that $doc documents:"
            printf '        %s\n' $zmiss
            rc=1
        fi
        if [ -n "$zextra" ]; then
            echo "::error::$zh documents reason tokens $doc does not:"
            printf '        %s\n' $zextra
            rc=1
        fi
    fi
fi

[ "$rc" -eq 0 ] && echo "OK: $(printf '%s\n' "$emitted" | grep -c .) reason tokens; engine, table and 简体中文 mirror agree"
exit $rc
