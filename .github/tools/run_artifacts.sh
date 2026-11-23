#!/usr/bin/env bash
# run_artifacts.sh <run> <glob> <dir> -- download the artifacts of a workflow run
# whose names match <glob>, one directory each below <dir>. A run that kept no
# such artifact leaves <dir> absent and exits 0; a run that kept one that cannot
# be read after three attempts exits 1. The caller tells "nothing to read" from
# "could not read" by that difference, never by a silenced failure: the e2e
# shards each divide the suite by what they read, and a shard that read less
# than the others divided it differently (CI, 2026-11-23). Reads
# GITHUB_REPOSITORY and GH_TOKEN.
set -uo pipefail
run="${1:?run_artifacts.sh: no run}"
glob="${2:?run_artifacts.sh: no name pattern}"
dir="${3:?run_artifacts.sh: no directory}"
repo="${GITHUB_REPOSITORY:?run_artifacts.sh: GITHUB_REPOSITORY is not set}"

names=""
for attempt in 1 2 3; do
    if names=$(gh api --paginate "repos/$repo/actions/runs/$run/artifacts?per_page=100" \
                 -q '.artifacts[] | select(.expired | not) | .name'); then
        break
    fi
    names=""
    [ "$attempt" = 3 ] && { echo "::error::the artifacts of run $run cannot be listed"; exit 1; }
    sleep 15
done

matched=0
while read -r name; do
    # shellcheck disable=SC2053  # the pattern is a glob by intent
    [[ -n "$name" && "$name" == $glob ]] && matched=1
done <<<"$names"
[ "$matched" = 1 ] || exit 0

for attempt in 1 2 3; do
    rm -rf "$dir"
    if gh run download "$run" -R "$repo" -p "$glob" -D "$dir"; then
        exit 0
    fi
    sleep 15
done
rm -rf "$dir"
echo "::error::run $run keeps artifacts matching '$glob', and they cannot be downloaded"
exit 1
