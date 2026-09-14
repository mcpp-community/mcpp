#!/usr/bin/env bash
# 682 -- `resolution.json` records the resolved dependency graph and
# `mcpp why deps` prints it (#634, X). Before, `why deps` printed only the
# lines of `mcpp.lock`, which does not record `path` dependencies, so a project
# of path dependencies showed "no mcpp.lock" after a successful build.
#
# Legs:
#   A. `why deps` lists the path dependency with the key and table that
#      requested it and its link form.
#   B. The record keeps every field it had: `schema_version` 2, `toolchain`,
#      `runtime`, and adds `graph` with the root first.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/fw/src" "$TMP/app/src"
printf '[package]\nnamespace = "demo"\nname = "fw"\nversion = "0.1.0"\n[targets.fw]\nkind = "lib"\n' > "$TMP/fw/mcpp.toml"
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > "$TMP/fw/src/fw.cppm"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../fw" }\n' > "$TMP/app/mcpp.toml"
printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$TMP/app/src/main.cpp"
cd "$TMP/app"

"$MCPP" build > build.log 2>&1 || fail "build failed" build.log

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" why deps > why.log 2>&1 || fail "A: why deps failed" why.log
grep -q "^dependency graph:" why.log || fail "A: no graph section" why.log
grep -q "demo.fw@0.1.0" why.log || fail "A: the path dependency is not listed" why.log
grep -qF "requested by mcpplibs.app@0.1.0 as 'demo.fw' in [dependencies]" why.log \
    || fail "A: the request is not listed" why.log
grep -q "linked static (default)" why.log || fail "A: the link form is not listed" why.log

# ── B ──────────────────────────────────────────────────────────────────────
json=$(find target -name resolution.json | head -1)
python3 - "$json" <<'PY' || fail "B: record shape" "$json"
import json, sys
doc = json.load(open(sys.argv[1]))
assert doc["schema_version"] == 2, doc["schema_version"]
assert "toolchain" in doc and "runtime" in doc, sorted(doc)
pkgs = doc["graph"]["packages"]
assert pkgs[0]["root"] is True and pkgs[0]["requested_by"] == [], pkgs[0]
fw = [p for p in pkgs if p["package"]["canonical"] == "demo.fw@0.1.0"]
assert len(fw) == 1 and fw[0]["package"]["source"].startswith("path+"), fw
PY

echo "OK"
