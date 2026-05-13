#!/usr/bin/env bash
# 39_xlings_index_migration.sh - legacy mcpp-index cache migrates to mcpplibs.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

export MCPP_HOME="$TMP/mcpp-home"

if [[ -z "${MCPP_VENDORED_XLINGS:-}" && -x "$HOME/.mcpp/registry/bin/xlings" ]]; then
    export MCPP_VENDORED_XLINGS="$HOME/.mcpp/registry/bin/xlings"
fi

source "$(dirname "$0")/_inherit_toolchain.sh"

cat > "$MCPP_HOME/config.toml" <<'TOML'
[xlings]
binary = "bundled"
home   = ""

[index]
default = "mcpp-index"

[index.repos."mcpp-index"]
url = "https://github.com/mcpp-community/mcpp-index.git"

[cache]
search_ttl_seconds = 3600

[build]
default_jobs    = 0
default_backend = "ninja"
TOML

mkdir -p "$MCPP_HOME/registry"
cat > "$MCPP_HOME/registry/.xlings.json" <<'JSON'
{
  "activeSubos": "default",
  "index_repos": [
    {
      "name": "mcpp-index",
      "url": "https://github.com/mcpp-community/mcpp-index.git"
    }
  ],
  "lang": "en",
  "mirror": "GLOBAL",
  "subos": {
    "default": {
      "dir": ""
    }
  }
}
JSON

"$MCPP" self env > "$TMP/env.log"

grep -q 'default = "mcpplibs"' "$MCPP_HOME/config.toml" || {
    cat "$MCPP_HOME/config.toml"; echo "config.toml default index was not migrated"; exit 1; }
grep -q '\[index.repos."mcpplibs"\]' "$MCPP_HOME/config.toml" || {
    cat "$MCPP_HOME/config.toml"; echo "config.toml repo table was not migrated"; exit 1; }
if grep -q 'default = "mcpp-index"' "$MCPP_HOME/config.toml" \
   || grep -q '\[index.repos."mcpp-index"\]' "$MCPP_HOME/config.toml"; then
    cat "$MCPP_HOME/config.toml"; echo "config.toml still contains legacy index key"; exit 1
fi

grep -q '"name": "mcpplibs"' "$MCPP_HOME/registry/.xlings.json" || {
    cat "$MCPP_HOME/registry/.xlings.json"; echo ".xlings.json repo name was not migrated"; exit 1; }
if grep -q '"name": "mcpp-index"' "$MCPP_HOME/registry/.xlings.json"; then
    cat "$MCPP_HOME/registry/.xlings.json"; echo ".xlings.json still contains legacy index name"; exit 1
fi

"$MCPP" self config > "$TMP/self-config.log"
grep -q 'mcpplibs' "$TMP/self-config.log" || {
    cat "$TMP/self-config.log"; echo "self config did not show mcpplibs"; exit 1; }
if grep -q 'index-repo.*mcpp-index[[:space:]]*:' "$TMP/self-config.log"; then
    cat "$TMP/self-config.log"; echo "self config still shows legacy index key"; exit 1
fi

echo "OK"
