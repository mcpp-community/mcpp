#!/usr/bin/env bash
# requires: python3
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
ART_CN='https://gitcode.com/xlings-res/mcpp-index'
# The default artifact is a region object since 2026.9.16.1 (#648 A6): the
# GitHub base for every mirror and the GitCode base a CN mirror asks first.
REGION="{ \"GLOBAL\": \"$ART\", \"CN\": \"$ART_CN\" }"
CFG="$MCPP_HOME/config.toml"
XJ="$MCPP_HOME/registry/.xlings.json"

# 1. Fresh init: new org URL + artifact declaration in both files.
# (xlings' own bootstrap may re-serialize .xlings.json with its writer, so
# assert on the key/value pairs, not on mcpp's seed line layout.)
"$MCPP" self env > /dev/null
grep -q "url      = \"$NEW_URL\"" "$CFG"  || { echo "config.toml missing new url"; exit 1; }
grep -qF "artifact = { GLOBAL = \"$ART\", CN = \"$ART_CN\" }" "$CFG" \
    || { echo "config.toml missing the region artifact"; cat "$CFG"; exit 1; }
grep -q "\"url\": \"$NEW_URL\"" "$XJ" \
    || { echo "seeded .xlings.json missing new url"; cat "$XJ"; exit 1; }
python3 - "$XJ" "$ART" "$ART_CN" <<'PYEOF' || { echo "seeded .xlings.json missing the region artifact"; cat "$XJ"; exit 1; }
import json, sys
doc = json.load(open(sys.argv[1]))
entry = next(r for r in doc["index_repos"] if r["name"] == "mcpplibs")
assert entry["artifact"] == {"GLOBAL": sys.argv[2], "CN": sys.argv[3]}, entry
PYEOF
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
grep -qF "\"url\": \"$NEW_URL\", \"artifact\": $REGION" "$XJ" \
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
grep -q 'gitcode.com/xlings-res/mcpp-index' "$XJ" || { echo "compact artifact has no CN half"; cat "$XJ"; exit 1; }

# 4. A home seeded before the region object (flat GitHub base, both spacings)
#    gains the CN half, once; a base the user wrote is left alone.
printf '{\n  "index_repos": [\n    { "name": "mcpplibs", "url": "%s", "artifact": "%s" }\n  ],\n  "mirror": "CN"\n}\n' "$NEW_URL" "$ART" > "$XJ"
"$MCPP" self env > /dev/null
grep -qF "\"artifact\": $REGION" "$XJ" || { echo "flat default artifact not upgraded"; cat "$XJ"; exit 1; }
cp "$XJ" "$TMP/xj4"; "$MCPP" self env > /dev/null
cmp -s "$XJ" "$TMP/xj4" || { echo "region upgrade not idempotent"; diff "$TMP/xj4" "$XJ"; exit 1; }
# A base the user states in config.toml owns the entry, and has no CN half.
cat > "$CFG" <<EOF
[index]
default = "mcpplibs"

[index.repos."mcpplibs"]
url = "$NEW_URL"
artifact = "https://mirror.example/idx"
EOF
"$MCPP" self env > /dev/null
grep -q 'https://mirror.example/idx' "$XJ" || { echo "a user artifact base was rewritten"; cat "$XJ"; exit 1; }
if grep -q 'gitcode.com/xlings-res/mcpp-index' "$XJ"; then echo "a user artifact base gained a CN half"; cat "$XJ"; exit 1; fi
if grep -q 'mcpp-community' "$XJ"; then
    echo "compact old org URL survived"; exit 1
fi

echo "OK"
