#!/usr/bin/env bash
# git_clone_retry.sh — `git clone`, but a hosted runner's DNS hiccup is not a
# red build.
#
# WHY THIS EXISTS
#
# Measured on a macOS ARM64 runner, in a step that had not yet executed a single
# line of mcpp:
#
#   Cloning into '/tmp/xlings-src'...
#   fatal: unable to access 'https://github.com/openxlings/xlings/':
#          Could not resolve host: github.com
#   ##[error]Process completed with exit code 128
#
# Thirty seconds of resolver timeout, then a failed PR check that says nothing
# about the change under test. Every workflow in this repository that reaches an
# external repository did it with a bare `git clone`, so every one of them could
# fail this way — six call sites, one failure mode.
#
# THIS REPOSITORY ALREADY LEARNED THIS FOR `curl`
#
# `fetch_release.sh` carries the same lesson one protocol over: `curl --retry`
# alone does not cover a transport-layer error, and `--retry-all-errors` is what
# does. `git` has no such flag at all — the retry has to be here.
#
# WHY BOUNDED, AND WHY IT DOES NOT TRY TO BE CLEVER
#
# git reports "the host does not resolve", "the repository does not exist" and
# "the credentials are wrong" as the same exit 128, with only the message to
# tell them apart. Parsing that message would be a second, weaker copy of git's
# own error taxonomy — and it would be wrong the first time git rephrased one.
#
# So this retries EVERY failure, a bounded number of times. A genuinely missing
# repository costs the attempts and then fails with git's own last message
# intact, which is ~50s and a diagnostic that still names the real cause. A
# resolver blip costs one backoff. That trade is the same one `--retry-all-errors`
# makes, and it is the right way round: a false red costs a maintainer a rerun
# and their attention, a slow true red costs 50 seconds.
#
# Usage:  git_clone_retry.sh [git clone args...] <url> <dir>
# Env:    GIT_CLONE_ATTEMPTS (default 4), GIT_CLONE_BACKOFF (default "5 15 30")
set -euo pipefail

ATTEMPTS="${GIT_CLONE_ATTEMPTS:-4}"
read -r -a BACKOFF <<< "${GIT_CLONE_BACKOFF:-5 15 30}"

# The destination is the last argument when it is not an option. Removing a
# partial clone before retrying matters: git refuses to clone into a directory
# that exists and is not empty, so without this the second attempt fails for a
# different reason than the first and the log stops making sense.
dest="${*: -1}"

for (( i = 1; i <= ATTEMPTS; i++ )); do
    # `cmd || rc=$?`, NOT `if cmd; then …; fi; rc=$?`.
    #
    # After an `if`, `$?` is the status of the IF STATEMENT — which is 0 when
    # the body did not run. Written that way this script exits 0 on a clone that
    # never succeeded, i.e. it turns a hard failure into a green step with a
    # missing checkout. Caught by asserting the exit code in the helper's own
    # test rather than by reading the output, which looked correct.
    rc=0
    git clone "$@" || rc=$?
    if (( rc == 0 )); then
        exit 0
    fi
    if (( i == ATTEMPTS )); then
        echo "git_clone_retry: giving up after $ATTEMPTS attempts (last exit $rc)" >&2
        exit "$rc"
    fi
    if [[ -n "$dest" && "$dest" != -* && -e "$dest" ]]; then
        echo "git_clone_retry: removing partial '$dest' before retrying" >&2
        rm -rf -- "$dest"
    fi
    delay="${BACKOFF[$(( i - 1 < ${#BACKOFF[@]} ? i - 1 : ${#BACKOFF[@]} - 1 ))]}"
    echo "git_clone_retry: attempt $i/$ATTEMPTS failed (exit $rc); retrying in ${delay}s" >&2
    sleep "$delay"
done
