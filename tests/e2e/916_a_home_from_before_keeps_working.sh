#!/usr/bin/env bash
# requires: unix-shell python3
# 916 -- the index layout of a home an earlier mcpp made is migrated
# (docs/25 §1).
#
# An earlier mcpp kept the data of its own index under the provider's data
# directory and wrote markers into payloads. This home is written by hand and
# holds no toolchain: whether a home the previous release made goes on
# building is the upgrade lane's question (.github/workflows/ci-upgrade.yml),
# which builds that home with the previous release itself. Criteria:
#   A. The first run moves the index data to <home>/indices/<name>, once, and
#      records its place and its store in the provider's configuration.
#   B. The index is read from there (doctor lists it), and the copy is not
#      read twice when a stale one reappears at the old place.
#   C. A payload installed under the old store, with the old marker, is left
#      exactly as it was.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
REG="$MCPP_HOME/registry"
mkdir -p "$REG/data/mcpplibs/pkgs/f"
printf '[index]\nspec = "1"\n' > "$REG/data/mcpplibs/index.toml"
printf 'package = { spec = "1", name = "fixture", type = "package" }\n' > "$REG/data/mcpplibs/pkgs/f/fixture.lua"
cat > "$REG/.xlings.json" <<JSON
{
  "index_repos": [
    { "name": "mcpplibs", "url": "https://github.com/mcpplibs/mcpp-index.git" }
  ],
  "lang": "en",
  "mirror": "auto"
}
JSON
OLD="$REG/data/xpkgs/mcpplibs-x-legacy/1.0.0"
mkdir -p "$OLD/src"
echo 'int legacy();' > "$OLD/src/legacy.h"
printf 'ok\n' > "$OLD/.mcpp_ok"
before=$(cd "$OLD" && find . -type f -exec sha256sum {} + | sort)

cd "$TMP"
"$MCPP" self config --mirror auto > a.log 2>&1 || fail "A: the first run failed" a.log
[ -f "$MCPP_HOME/indices/mcpplibs/pkgs/f/fixture.lua" ] || fail "A: the index data did not move"
[ ! -e "$REG/data/mcpplibs" ] || fail "A: the index data is still at the old place"
python3 - "$REG/.xlings.json" "$MCPP_HOME" <<'PY' || fail "A: the configuration does not record the index's place" "$REG/.xlings.json"
import json, os, sys
doc = json.load(open(sys.argv[1]))
e = next(r for r in doc["index_repos"] if r["name"] == "mcpplibs")
home = sys.argv[2]
assert os.path.realpath(e["dir"]) == os.path.realpath(os.path.join(home, "indices", "mcpplibs")), e
assert os.path.realpath(e["store"]) == os.path.realpath(os.path.join(home, "xpkgs")), e
PY
echo "ok: A. the index moved once and its place is recorded"

"$MCPP" self doctor > b.log 2>&1 || true
[ "$(grep -c "index 'mcpplibs'" b.log)" = 1 ] || fail "B: doctor does not list the index once" b.log
mkdir -p "$REG/data/mcpplibs/pkgs"
"$MCPP" self doctor > b2.log 2>&1 || true
[ "$(grep -c "index 'mcpplibs'" b2.log)" = 1 ] || fail "B: a stale copy at the old place is read as a second index" b2.log
echo "ok: B. the index is read where it is, once"

after=$(cd "$OLD" && find . -type f -exec sha256sum {} + | sort)
[ "$before" = "$after" ] || fail "C: a payload of the old store changed"
echo "ok: C. the old store's payload is as it was"

echo "PASS: 916_a_home_from_before_keeps_working"
