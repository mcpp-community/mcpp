#!/usr/bin/env bash
# requires:
# 1067 — a home that borrows another's registry (`[xlings] home`) leaves the
# places of the registry's indices as their owner recorded them.
#
# Before, every home that started over a registry wrote its own index
# directory and store into the registry's `.xlings.json`. The e2e homes under
# a temporary directory did, and a later build over the shared registry was
# sent to a store that had been deleted with its home ("install path missing
# after fetch", measured on Windows and macOS).
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

export MCPP_HOME="$TMP/owner"
"$MCPP" index list > "$TMP/owner.log" 2>&1 || { cat "$TMP/owner.log"; echo "FAIL: the owner home did not start"; exit 1; }
REGISTRY="$TMP/owner/registry"
test -f "$REGISTRY/.xlings.json" || { echo "FAIL: the owner home wrote no registry configuration"; exit 1; }
grep -q '"store"' "$REGISTRY/.xlings.json" || {
    cat "$REGISTRY/.xlings.json"; echo "FAIL: the owner recorded no store for its index"; exit 1; }
cp "$REGISTRY/.xlings.json" "$TMP/before.json"

export MCPP_HOME="$TMP/borrower"
mkdir -p "$MCPP_HOME"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$(host_path "$REGISTRY")"
EOF
"$MCPP" index list > "$TMP/borrower.log" 2>&1 || {
    cat "$TMP/borrower.log"; echo "FAIL: the borrowing home did not start"; exit 1; }

cmp -s "$TMP/before.json" "$REGISTRY/.xlings.json" || {
    diff "$TMP/before.json" "$REGISTRY/.xlings.json" || true
    echo "FAIL: the borrowing home rewrote the registry's index places"; exit 1; }
if grep -q 'borrower' "$REGISTRY/.xlings.json"; then
    cat "$REGISTRY/.xlings.json"; echo "FAIL: the registry names the borrowing home"; exit 1
fi

echo "PASS: 1067 a borrowed registry keeps its owner's places"
