#!/usr/bin/env bash
# `mcpp index list` shows configured registries (after init).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

# init
"$MCPP" self env > /dev/null

# list should at minimum show mcpp-index (from seeded config)
out=$("$MCPP" index list 2>&1)
[[ "$out" == *"mcpp-index"* ]] || { echo "index list missing mcpp-index: $out"; exit 1; }

echo "OK"
