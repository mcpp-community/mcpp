#!/usr/bin/env sh
# check.sh <stamp> <file> [-p <compile-db>]
#
# A stand-in for clang-tidy so this example runs anywhere. It enforces one real
# rule and then TOUCHES THE STAMP — a check action's output is a stamp and the
# command has to create it, which is the ergonomic gap a rule package closes.
set -eu
stamp="$1"; file="$2"
if grep -n 'goto ' "$file"; then
    echo "check: $file uses goto" >&2
    exit 1
fi
mkdir -p "$(dirname "$stamp")"
: > "$stamp"
