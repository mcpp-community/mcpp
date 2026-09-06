#!/usr/bin/env bash
# requires: unix-shell
# Every example the documentation lists exists, and every example that exists is
# listed.
#
# WHY THIS IS WORTH A TEST. Nothing in CI builds an example, so the curriculum
# is the one part of this repository a rename can break silently: the docs keep
# pointing at a path that is gone, and every job stays green. The four device
# examples were regrouped under `examples/09-heterogeneous/` in 2026.9.6.2, and
# that move touched 14 files by hand.
#
# THE CHECK IS BIDIRECTIONAL, and one direction alone would not be worth
# running. "Every listed path exists" passes on a document that lists nothing;
# "every example is listed" passes on a document that lists everything and
# points half of it at the wrong place. Both together are the property.
#
# It is a structural check and says so: it does not build anything, so it
# cannot tell whether an example still works.
set -e

ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
fails=0
fail() { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }

for doc in "$ROOT/docs/01-examples.md" "$ROOT/docs/zh/01-examples.md"; do
    [ -f "$doc" ] || { fail "missing $doc"; continue; }
    printf -- '--- %s ---\n' "${doc#"$ROOT"/}"

    # The link targets, not the code spans: a link is what a reader follows.
    listed=$(grep -oE '\]\(\.\.(/\.\.)?/examples/[A-Za-z0-9._/-]+\)' "$doc" \
             | sed -E 's#.*/examples/##; s#/?\)$##' | sort -u)
    [ -n "$listed" ] || fail "$doc lists no example link at all"
    for path in $listed; do
        if [ -d "$ROOT/examples/$path" ]; then
            printf 'ok: listed examples/%s exists\n' "$path"
        else
            fail "$doc links examples/$path, which does not exist"
        fi
    done

    # And the other direction: every project on disk is reachable from the doc.
    # A project is a directory containing an mcpp.toml, at any depth under
    # examples/ -- which is what makes the grouped layout expressible.
    while IFS= read -r manifest; do
        rel=$(dirname "${manifest#"$ROOT"/examples/}")
        # `<group>/<model>/app` is listed as `<group>/<model>`; accept any
        # ancestor, because the doc links the lesson and not its build root.
        #
        # WHOLE LINES, NOT A SUBSTRING. `case "$listed" in *"$probe"*)` would
        # call `04-work` reachable because `04-workspace` is listed, and would
        # keep doing so as names grew closer together. The set is a list of
        # paths; membership in it is an equality on one of them.
        found=0
        probe="$rel"
        while [ -n "$probe" ] && [ "$probe" != "." ]; do
            if printf '%s\n' "$listed" | grep -Fxq "$probe"; then
                found=1; break
            fi
            probe=$(dirname "$probe")
        done
        if [ "$found" -eq 1 ]; then
            printf 'ok: examples/%s is reachable from the list\n' "$rel"
        else
            fail "examples/$rel has an mcpp.toml and $doc never links it"
        fi
    done <<EOF
$(find "$ROOT/examples" -name mcpp.toml -not -path '*/target/*' | sort)
EOF
done

if [ "$fails" -ne 0 ]; then
    printf 'FAIL: %s assertion(s) failed\n' "$fails"
    exit 1
fi
printf 'PASS: the examples on disk and the ones the docs list are the same set\n'
