#!/usr/bin/env bash
# 681 -- `[index.repos.<name>]` in config.toml reaches a registry that already
# exists (#634, C4). The table used to seed the registry's `.xlings.json` only
# when that file did not exist, so a table added to a home that had run once
# did nothing, without a word. xlings follows the file at its next index sync
# (measured separately: the index directory became the named checkout); this
# test holds the part mcpp owns, which is the file.
#
# Legs:
#   A. A table added to an existing home is written into `index_repos`, with
#      one line saying so, and the change is recorded.
#   B. A second run changes nothing and prints nothing about the index.
#   C. Removing the table restores the registry's previous state.
#   D. An entry changed by someone else after mcpp wrote it is left alone when
#      the table is removed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
CFG="$MCPP_HOME/config.toml"
XJ="$MCPP_HOME/registry/.xlings.json"
REC="$MCPP_HOME/registry/.mcpp-index-overrides.json"
CHECKOUT="$TMP/xim-checkout"
mkdir -p "$CHECKOUT"
cd "$TMP"

entry_url() {  # entry_url <name> -> url of that index_repos entry, or "absent"
    python3 - "$XJ" "$1" <<'PY'
import json, sys
repos = json.load(open(sys.argv[1])).get("index_repos", [])
urls = [r.get("url") for r in repos if r.get("name") == sys.argv[2]]
print(urls[0] if urls else "absent")
PY
}

"$MCPP" self env > /dev/null 2>&1 || true
[ -f "$XJ" ] || fail "the registry was not seeded"
[ "$(entry_url xim)" = "absent" ] || fail "a fresh home already names xim" "$XJ"

# ── A ──────────────────────────────────────────────────────────────────────
printf '\n[index.repos.xim]\nurl = "%s"\n' "$CHECKOUT" >> "$CFG"
"$MCPP" self env > a.log 2>&1 || true
[ "$(entry_url xim)" = "$CHECKOUT" ] || fail "A: the table did not reach .xlings.json" "$XJ" a.log
grep -qF "xim -> $CHECKOUT ([index.repos.xim] in config.toml)" a.log || fail "A: no line names the change" a.log
[ -f "$REC" ] || fail "A: the change was not recorded"

# ── B ──────────────────────────────────────────────────────────────────────
cp "$XJ" xj.before
"$MCPP" self env > b.log 2>&1 || true
cmp -s "$XJ" xj.before || fail "B: a second run changed .xlings.json" "$XJ"
if grep -q "xim -> " b.log; then fail "B: a second run printed the index line again" b.log; fi

# ── C ──────────────────────────────────────────────────────────────────────
python3 - "$CFG" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
open(p, "w").write(re.sub(r'\n\[index\.repos\.xim\]\nurl = "[^"]*"\n', '\n', s))
PY
"$MCPP" self env > c.log 2>&1 || true
[ "$(entry_url xim)" = "absent" ] || fail "C: removing the table left the entry" "$XJ" c.log
grep -q "xim restored" c.log || fail "C: no line names the restoration" c.log
[ ! -f "$REC" ] || fail "C: the record outlived the table" "$REC"

# ── D ──────────────────────────────────────────────────────────────────────
printf '\n[index.repos.xim]\nurl = "%s"\n' "$CHECKOUT" >> "$CFG"
"$MCPP" self env > /dev/null 2>&1 || true
python3 - "$XJ" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
for r in d["index_repos"]:
    if r.get("name") == "xim": r["url"] = "/somewhere/else"
json.dump(d, open(p, "w"), indent=2)
PY
python3 - "$CFG" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
open(p, "w").write(re.sub(r'\n\[index\.repos\.xim\]\nurl = "[^"]*"\n', '\n', s))
PY
"$MCPP" self env > d.log 2>&1 || true
[ "$(entry_url xim)" = "/somewhere/else" ] || fail "D: an entry someone else changed was overwritten" "$XJ"

echo "OK"
