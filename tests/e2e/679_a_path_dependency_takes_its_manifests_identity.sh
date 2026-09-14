#!/usr/bin/env bash
# 679 -- a `path` or `git` dependency's identity is the one its manifest
# declares, whatever key reached it (#634, A2; SPEC-001 §1.2).
#
# Before, only the short name was compared: `fw` reaching a manifest that
# declares `huxdemo.fw` resolved as `mcpplibs.fw`, and a second edge keyed
# `huxdemo.fw` over the same directory put the same module into the build
# twice, which the scanner then refused naming one file twice.
#
# Legs:
#   A. Two edges over one directory, keyed `huxdemo.fw` (the application) and
#      `fw` (a component): the build succeeds, the unit is compiled once, one
#      warning names the component's key, its normalisation and the declared
#      identity, and the resolution record shows one package with both keys.
#   B. Keys that match the declaration warn nothing.
#   C. Two keys over a manifest that declares no namespace are two identities
#      over one source, and are refused before scanning.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/fw/src" "$TMP/comp/src" "$TMP/app/src" "$TMP/plain/src"
cat > "$TMP/fw/mcpp.toml" <<'TOML'
[package]
namespace = "huxdemo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
TOML
printf 'export module fw;\nexport int fw_anchor() { return 7; }\n' > "$TMP/fw/src/fw.cppm"
cat > "$TMP/comp/mcpp.toml" <<'TOML'
[package]
namespace = "huxdemo"
name      = "comp"
version   = "0.1.0"
[targets.comp]
kind = "lib"
[dependencies]
fw = { path = "../fw" }
TOML
printf 'export module comp;\nimport fw;\nexport int comp_anchor() { return fw_anchor(); }\n' > "$TMP/comp/src/comp.cppm"
cat > "$TMP/app/mcpp.toml" <<'TOML'
[package]
name    = "app"
version = "0.1.0"
[dependencies]
huxdemo.fw   = { path = "../fw" }
huxdemo.comp = { path = "../comp" }
TOML
printf 'import fw;\nimport comp;\nint main() { return fw_anchor() + comp_anchor() == 14 ? 0 : 1; }\n' > "$TMP/app/src/main.cpp"

# ── A ──────────────────────────────────────────────────────────────────────
( cd "$TMP/app" && "$MCPP" build > build.log 2>&1 ) || fail "A: build failed" "$TMP/app/build.log"
( cd "$TMP/app" && "$MCPP" run > run.log 2>&1 ) || fail "A: the program did not exit 0" "$TMP/app/run.log"
n=$(grep -c "that identity is used" "$TMP/app/build.log" || true)
[ "$n" = "1" ] || fail "A: expected one identity warning, saw $n" "$TMP/app/build.log"
grep "that identity is used" "$TMP/app/build.log" | grep -q "'fw'" \
    && grep "that identity is used" "$TMP/app/build.log" | grep -q "mcpplibs.fw" \
    && grep "that identity is used" "$TMP/app/build.log" | grep -q "huxdemo.fw" \
    || fail "A: the warning does not name the key, its normalisation and the declaration" "$TMP/app/build.log"
units=$(find "$TMP/app/target" -path '*/obj/*' -name 'fw.m.o' | wc -l | tr -d ' ')
[ "$units" = "1" ] || fail "A: fw.cppm compiled $units times"
json=$(find "$TMP/app/target" -name resolution.json | head -1)
keys=$(python3 - "$json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
fw = [p for p in doc["graph"]["packages"] if p["package"]["name"] == "fw"]
print(len(fw), " ".join(sorted(r["key"] for r in fw[0]["requested_by"])) if fw else "")
PY
)
[ "$keys" = "1 fw huxdemo.fw" ] || fail "A: graph record reads '$keys'" "$json"

# ── B ──────────────────────────────────────────────────────────────────────
sed 's/^fw = { path/huxdemo.fw = { path/' "$TMP/comp/mcpp.toml" > "$TMP/comp/mcpp.toml.new"
mv "$TMP/comp/mcpp.toml.new" "$TMP/comp/mcpp.toml"
rm -rf "$TMP/app/target"
( cd "$TMP/app" && "$MCPP" build > build2.log 2>&1 ) || fail "B: build failed" "$TMP/app/build2.log"
if grep -q "that identity is used" "$TMP/app/build2.log"; then
    fail "B: matching keys produced an identity warning" "$TMP/app/build2.log"
fi

# ── C ──────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/nons/fw/src" "$TMP/nons/app/src"
printf '[package]\nname = "fw"\nversion = "0.1.0"\n[targets.fw]\nkind = "lib"\n' > "$TMP/nons/fw/mcpp.toml"
printf 'export module fw;\nexport int fw_anchor() { return 7; }\n' > "$TMP/nons/fw/src/fw.cppm"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\na.fw = { path = "../fw" }\nb.fw = { path = "../fw" }\n' > "$TMP/nons/app/mcpp.toml"
printf 'import fw;\nint main() { return 0; }\n' > "$TMP/nons/app/src/main.cpp"
if ( cd "$TMP/nons/app" && "$MCPP" build > build.log 2>&1 ); then
    fail "C: two identities over one source built" "$TMP/nons/app/build.log"
fi
grep -q "one source is reached as two packages" "$TMP/nons/app/build.log" \
    || fail "C: the refusal does not name the two identities" "$TMP/nons/app/build.log"
if grep -q "already provided\|is provided by package" "$TMP/nons/app/build.log"; then
    fail "C: the refusal came from the scanner, not before scanning" "$TMP/nons/app/build.log"
fi

echo "OK"
