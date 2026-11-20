#!/usr/bin/env bash
# main_run.sh -- print the run whose test artifacts are main's: main's last
# successful ci.yml run, or, when that run registered a verified run instead of
# testing (design 2026-11-20 §5.22), the run it registered. Prints nothing when
# main has no successful run. Reads GITHUB_REPOSITORY and GH_TOKEN.
#
# A push to main whose tree a `full` run of its pull request verified runs no
# test: its `register` job uploads `registered`, a file holding the verifying
# run's id. The stores, the unit archives and the build directories later runs
# read from main are that run's.
set -uo pipefail
repo="${GITHUB_REPOSITORY:?main_run.sh: GITHUB_REPOSITORY is not set}"
run=$(gh run list -R "$repo" --workflow ci.yml --branch main --status success --limit 1 \
        --json databaseId -q '.[0].databaseId' 2>/dev/null || true)
[ -n "$run" ] || exit 0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if gh run download "$run" -R "$repo" -n registered -D "$tmp" > /dev/null 2>&1 && [ -s "$tmp/run" ]; then
    tr -d '[:space:]' < "$tmp/run"
    echo
else
    echo "$run"
fi
