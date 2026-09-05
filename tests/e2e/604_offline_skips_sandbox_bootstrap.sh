#!/usr/bin/env bash
# `--offline` promises never to touch the network, and a fresh home is where
# that promise was being broken: the sandbox bootstrap that `load_or_init` runs
# on first use clones the package index and installs ninja and patchelf through
# `xlings install`. Measured before the gate, `MCPP_OFFLINE=1 mcpp self doctor`
# in an empty home spent 26 seconds and wrote 126 MB before its first check.
#
# Under offline mode the three network-bound steps are skipped, once, visibly,
# and the command goes on to its remaining checks. A home that is already
# bootstrapped says nothing, because there is nothing to skip.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ── A fresh home under offline mode is left un-bootstrapped ─────────────
out="$TMP/fresh.log"
MCPP_HOME="$TMP/home" MCPP_OFFLINE=1 "$MCPP" self doctor > "$out" 2>&1 || true

n=$(grep -c "Skipping sandbox bootstrap (offline mode" "$out" || true)
[[ "$n" -eq 1 ]] || {
    cat "$out"; echo "FAIL: expected the skip to be announced once, saw $n"; exit 1; }

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
echo "PASS: a fresh home under offline mode is left un-bootstrapped, audibly"

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
