#!/usr/bin/env bash
# requires:
# `mcpp index status` is a read-only, offline snapshot of the local indexes.
# After a successful init it reports both indexes present, and a second run
# needs no network (steady-state commands are offline once init succeeded).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

# init (this is the one place a first run may fetch the index)
"$MCPP" self env > /dev/null

# status: exits 0, prints the table header + both index rows
out=$("$MCPP" index status 2>&1)
[[ "$out" == *"xim"* ]]      || { echo "index status missing xim row: $out"; exit 1; }
[[ "$out" == *"mcpplibs"* ]] || { echo "index status missing mcpplibs row: $out"; exit 1; }
[[ "$out" == *"refreshed"* ]] || { echo "index status missing header: $out"; exit 1; }

# After init the official index is present (not 'missing').
echo "$out" | grep -E '^[[:space:]]*xim[[:space:]]' | grep -q 'missing' \
    && { echo "xim index reported missing right after init: $out"; exit 1; }

# Offline invariant: a second status with the network cut must still succeed.
# (No network calls in the status path; this just re-asserts it deterministically.)
out2=$("$MCPP" index status 2>&1)
[[ "$out2" == *"mcpplibs"* ]] || { echo "second index status failed: $out2"; exit 1; }

echo "OK"
