#!/usr/bin/env sh
# check.sh <file> [-p <compile-db>]
#
# A stand-in for clang-tidy: it reads a file and answers with an EXIT CODE.
#
# It does not touch the stamp, and that is the point. A check action's output
# is bookkeeping the build graph needs, not something an analyser knows about —
# clang-tidy writes nothing on success either. mcpp creates the stamp when the
# command succeeds, which is what makes a check rule writable without a
# per-platform wrapper.
set -eu
file="$1"
if grep -n 'goto ' "$file"; then
    echo "check: $file uses goto" >&2
    exit 1
fi
