#!/usr/bin/env bash
# THE PROGRAM A BUILD MADE, AS THE BUILD REPORTS IT.
#
#   built_artifact.sh <mcpp> <name> [build arguments...]
#
# Runs `<mcpp> build --message-format json [arguments]` and prints the absolute
# path of the artifact named <name>, from the build's `artifact` events
# (docs/50-machine-output.md §9): one line per product, whose object has one
# `path`. Read with grep and sed (`-E`, as GNU and BSD sed read it), so that a
# machine with no JSON reader (the hermetic container) can ask it. The build's narration stays on standard
# error. A failed build, or one that reports no such artifact, exits non-zero.
#
# Instead of a search of target/: a self-hosting build keeps the engine it
# runs under target/<triple>/<fingerprint>/engine/epoch-N/mcpp, and a search
# for a file named mcpp took that copy (an x86_64 program) for the product of
# an aarch64 build (2026-11-14).
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 <mcpp> <name> [build arguments...]" >&2; exit 2; }
mcpp=$1; name=$2; shift 2
report=$(mktemp)
trap 'rm -f "$report"' EXIT
"$mcpp" build --message-format json "$@" > "$report"
found=$(grep -F '"event":"artifact"' "$report" | grep -F "\"name\":\"$name\"" \
          | sed -nE 's/.*"path":"(([^"\\]|\\.)*)".*/\1/p' | tail -1 | sed 's/\\\\/\\/g' || true)
[ -n "$found" ] || { echo "the build reported no artifact named $name" >&2; exit 1; }
printf '%s\n' "$found"
