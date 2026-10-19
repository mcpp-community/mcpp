#!/usr/bin/env bash
# Doctor reads local state without bootstrapping, on cold and warm homes.
# Offline mode must reach later diagnostics without creating the cold home.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ── A fresh home under offline mode is left un-bootstrapped ─────────────
out="$TMP/fresh.log"
MCPP_HOME="$TMP/home" MCPP_OFFLINE=1 "$MCPP" self doctor > "$out" 2>&1 || true

[[ ! -e "$TMP/home" ]] || {
    cat "$out"; echo "FAIL: an offline diagnostic initialized the home"; exit 1; }

# The index clone is the first network step and the largest; its absence is
# the structural evidence that nothing was fetched.
[[ ! -e "$TMP/home/registry/data/xim-pkgindex" ]] || {
    echo "FAIL: the package index was cloned under offline mode"; exit 1; }
[[ ! -e "$TMP/home/registry/subos/default/.xlings.json" ]] || {
    echo "FAIL: the sandbox was initialised under offline mode"; exit 1; }

# Skipping the bootstrap must not end the command: the checks after the
# registry one still run.
grep -q "Checking build policy" "$out" || {
    cat "$out"; echo "FAIL: the doctor stopped at the registry check"; exit 1; }
echo "PASS: a fresh home under offline mode stays untouched and diagnosis continues"

# ── Control: a bootstrapped home has nothing to skip and says nothing ───
#
# Without this, a doctor that printed the sentence on every offline run would
# pass the count above. The e2e runner's own home is bootstrapped by the time
# this script runs.
out2="$TMP/warm.log"
MCPP_OFFLINE=1 "$MCPP" self doctor > "$out2" 2>&1 || true
if grep -q "Skipping sandbox bootstrap" "$out2"; then
    cat "$out2"; echo "FAIL: a bootstrapped home announced a skip"; exit 1
fi
echo "PASS: a bootstrapped home under offline mode announces nothing"

echo "PASS: offline skips the sandbox bootstrap"
