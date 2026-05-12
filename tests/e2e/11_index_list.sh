#!/usr/bin/env bash
# `mcpp index list` shows configured registries (after init).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

# init
"$MCPP" self env > /dev/null

# list should at minimum show mcpplibs (from seeded config)
out=$("$MCPP" index list 2>&1)
[[ "$out" == *"mcpplibs"* ]] || { echo "index list missing mcpplibs: $out"; exit 1; }

echo "OK"
