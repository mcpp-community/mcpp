#!/usr/bin/env bash
# requires: elf
# 677 -- a matching `[target.<selector>.dependencies]` declaration REPLACES the
# unconditional declaration of the same identity on the rows the selector
# matches (#634, A1). Before, the merge kept the unconditional entry, so a
# conditional `linkage = "shared"` was dropped on its own row without a word.
#
# Legs:
#   A. The conditional table matches this row: `libfw.so` is built and the
#      program NEEDs it, and the resolution record names the conditional table.
#   B. The conditional table does not match: the unconditional declaration
#      stands (a static link), and the record names `[dependencies]`.
#   C. The modifier-only spelling `demo.fw = { linkage = "shared" }` names no
#      source. It still fails, and the warning names the conditional table and
#      restates the dependency with the source the unconditional table wrote.
#   D. A misspelled sub-table of `[target.<selector>]` is reported instead of
#      doing nothing.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# table_of <resolution.json> <canonical> -> the tables its requests came from
table_of() {
    python3 - "$1" "$2" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
for p in doc["graph"]["packages"]:
    if p["package"]["canonical"] == sys.argv[2]:
        print(" ".join(r["table"] for r in p["requested_by"]))
PY
}

mkdir -p "$TMP/fw/src"
cat > "$TMP/fw/mcpp.toml" <<'TOML'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
TOML
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > "$TMP/fw/src/fw.cppm"

make_app() {  # make_app <dir> <tail of mcpp.toml>
    mkdir -p "$TMP/$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n%s' "$1" "$2" > "$TMP/$1/mcpp.toml"
    printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$TMP/$1/src/main.cpp"
}

# ── A ──────────────────────────────────────────────────────────────────────
make_app matching '[dependencies]
demo.fw = { path = "../fw" }
[target.'"'"'cfg(os = "linux")'"'"'.dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
'
( cd "$TMP/matching" && "$MCPP" build > build.log 2>&1 ) || fail "A: build failed" "$TMP/matching/build.log"
so=$(find "$TMP/matching/target" -name libfw.so | head -1)
[ -n "$so" ] || fail "A: the conditional linkage did not produce libfw.so" "$TMP/matching/build.log"
exe=$(find "$TMP/matching/target" -type f -name matching -perm -u+x | head -1)
readelf -d "$exe" | grep -q 'NEEDED.*libfw.so' || fail "A: the program does not NEED libfw.so"
json=$(find "$TMP/matching/target" -name resolution.json | head -1)
t=$(table_of "$json" "demo.fw@0.1.0")
[ "$t" = "[target.'cfg(os = \"linux\")'.dependencies]" ] || fail "A: graph table is '$t'" "$json"

# ── B ──────────────────────────────────────────────────────────────────────
make_app other '[dependencies]
demo.fw = { path = "../fw" }
[target.'"'"'cfg(os = "windows")'"'"'.dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
'
( cd "$TMP/other" && "$MCPP" build > build.log 2>&1 ) || fail "B: build failed" "$TMP/other/build.log"
[ -z "$(find "$TMP/other/target" -name libfw.so | head -1)" ] || fail "B: a non-matching selector produced libfw.so"
json=$(find "$TMP/other/target" -name resolution.json | head -1)
t=$(table_of "$json" "demo.fw@0.1.0")
[ "$t" = "[dependencies]" ] || fail "B: graph table is '$t'" "$json"

# ── C ──────────────────────────────────────────────────────────────────────
make_app modifier '[dependencies]
demo.fw = { path = "../fw" }
[target.'"'"'cfg(os = "linux")'"'"'.dependencies]
demo.fw = { linkage = "shared" }
'
if ( cd "$TMP/modifier" && "$MCPP" build > build.log 2>&1 ); then
    fail "C: a table that names no source built" "$TMP/modifier/build.log"
fi
grep -qF "[target.'cfg(os = \"linux\")'.dependencies] demo.fw.linkage = \"shared\"" "$TMP/modifier/build.log" \
    || fail "C: the warning does not name the conditional table" "$TMP/modifier/build.log"
grep -qF 'demo.fw = { path = "../fw", linkage = "shared" }' "$TMP/modifier/build.log" \
    || fail "C: the warning does not restate the dependency with its source" "$TMP/modifier/build.log"

# ── D ──────────────────────────────────────────────────────────────────────
make_app typo '[dependencies]
demo.fw = { path = "../fw" }
[target.'"'"'cfg(os = "linux")'"'"'.dependecies]
demo.fw = { path = "../fw", linkage = "shared" }
'
( cd "$TMP/typo" && "$MCPP" build > build.log 2>&1 ) || fail "D: build failed" "$TMP/typo/build.log"
grep -qF "[target.'cfg(os = \"linux\")'.dependecies] is not a section mcpp reads" "$TMP/typo/build.log" \
    || fail "D: the misspelled section was not reported" "$TMP/typo/build.log"

echo "OK"
