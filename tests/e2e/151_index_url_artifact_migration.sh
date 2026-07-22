#!/usr/bin/env bash
# #267/#269: fresh init seeds the mcpplibs-org index URL + artifact source;
# legacy config.toml / .xlings.json (old org URL, no artifact) are healed in
# place, idempotently, in both pretty (mcpp writer) and compact (xlings
# writer) JSON spacings. Unrelated .xlings.json state must survive.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

NEW_URL='https://github.com/mcpplibs/mcpp-index.git'
OLD_URL='https://github.com/mcpp-community/mcpp-index.git'
ART='https://github.com/xlings-res/mcpp-index'
CFG="$MCPP_HOME/config.toml"
XJ="$MCPP_HOME/registry/.xlings.json"

# 1. Fresh init: new org URL + artifact declaration in both files.
# (xlings' own bootstrap may re-serialize .xlings.json with its writer, so
# assert on the key/value pairs, not on mcpp's seed line layout.)
"$MCPP" self env > /dev/null
grep -q "url      = \"$NEW_URL\"" "$CFG"  || { echo "config.toml missing new url"; exit 1; }
grep -q "artifact = \"$ART\"" "$CFG"      || { echo "config.toml missing artifact"; exit 1; }
grep -q "\"url\": \"$NEW_URL\"" "$XJ" \
    || { echo "seeded .xlings.json missing new url"; cat "$XJ"; exit 1; }
grep -q "\"artifact\": \"$ART\"" "$XJ" \
    || { echo "seeded .xlings.json missing artifact"; cat "$XJ"; exit 1; }
if grep -q 'mcpp-community/mcpp-index' "$CFG" "$XJ"; then
    echo "old org URL leaked into fresh seed"; exit 1
fi

# 2. Legacy heal (pretty spacing): old org URL, no artifact.
cat > "$CFG" <<EOF
[xlings]
binary = "bundled"
home   = ""

[index]
default = "mcpplibs"

[index.repos."mcpplibs"]
url = "$OLD_URL"
EOF
cat > "$XJ" <<EOF
{
  "index_repos": [
    { "name": "mcpplibs", "url": "$OLD_URL" }
  ],
  "subos": "default",
  "lang": "en",
  "mirror": "auto"
}
EOF
"$MCPP" self env > /dev/null
grep -q "url = \"$NEW_URL\"" "$CFG"       || { echo "config.toml url not healed"; cat "$CFG"; exit 1; }
grep -q "\"url\": \"$NEW_URL\", \"artifact\": \"$ART\"" "$XJ" \
    || { echo ".xlings.json not healed"; cat "$XJ"; exit 1; }
grep -q '"subos": "default"' "$XJ"        || { echo "unrelated .xlings.json key lost"; exit 1; }
if grep -q 'mcpp-community/mcpp-index' "$CFG" "$XJ"; then
    echo "old org URL survived heal"; exit 1
fi

# Idempotent: a second run must not touch either file again.
cp "$CFG" "$TMP/cfg1"; cp "$XJ" "$TMP/xj1"
"$MCPP" self env > /dev/null
cmp -s "$CFG" "$TMP/cfg1" || { echo "config.toml heal not idempotent"; exit 1; }
cmp -s "$XJ" "$TMP/xj1"   || { echo ".xlings.json heal not idempotent"; exit 1; }

# 3. Legacy heal (compact spacing, old index name): xlings-writer format.
printf '{"index_repos":[{"name":"mcpp-index","url":"%s"}],"mirror":"auto"}' "$OLD_URL" > "$XJ"
"$MCPP" self env > /dev/null
grep -q '"name":"mcpplibs"' "$XJ"      || { echo "compact name not healed"; cat "$XJ"; exit 1; }
grep -q 'xlings-res/mcpp-index' "$XJ"  || { echo "compact artifact not injected"; cat "$XJ"; exit 1; }
if grep -q 'mcpp-community' "$XJ"; then
    echo "compact old org URL survived"; exit 1
fi

echo "OK"
