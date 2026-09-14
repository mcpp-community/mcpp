#!/usr/bin/env bash
# requires: elf
# 678 -- `[target.<selector>.targets.<name>] kind` is the per-row form of
# `[targets.<name>] kind` (#634, A1): a framework linked statically on one row
# states on another that it must be one shared copy, in its own manifest,
# and every consumer keeps a single unconditional dependency line.
#
# Legs:
#   A. The row matches: a consumer that asks for nothing gets `libfw.so`, and
#      the resolution record gives the reason `row-kind`.
#   B. The row does not match: a static link, reason `default`.
#   C. A consumer that asks for `linkage = "static"` is warned, and the warning
#      names the row's statement; `--strict` turns it into a failure.
#   D. A row that names no target, a program target, or a non-library kind is
#      refused.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

reason_of() {  # reason_of <resolution.json> <canonical>
    python3 - "$1" "$2" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
for p in doc["graph"]["packages"]:
    if p["package"]["canonical"] == sys.argv[2]:
        print(p.get("link", {}).get("form", ""), p.get("link", {}).get("reason", ""))
PY
}

make_fw() {  # make_fw <dir> <os in the selector> [extra manifest text]
    mkdir -p "$TMP/$1/src"
    cat > "$TMP/$1/mcpp.toml" <<TOML
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
[target.'cfg(os = "$2")'.targets.fw]
kind = "shared"
$3
TOML
    printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > "$TMP/$1/src/fw.cppm"
}
make_app() {  # make_app <dir> <fw dir> [edge options]
    mkdir -p "$TMP/$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../%s"%s }\n' \
        "$1" "$2" "$3" > "$TMP/$1/mcpp.toml"
    printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$TMP/$1/src/main.cpp"
}

make_fw fwlinux linux
make_fw fwwin windows

# ── A ──────────────────────────────────────────────────────────────────────
make_app a fwlinux
( cd "$TMP/a" && "$MCPP" build > build.log 2>&1 ) || fail "A: build failed" "$TMP/a/build.log"
[ -n "$(find "$TMP/a/target" -name libfw.so | head -1)" ] || fail "A: no libfw.so" "$TMP/a/build.log"
r=$(reason_of "$(find "$TMP/a/target" -name resolution.json | head -1)" "demo.fw@0.1.0")
[ "$r" = "shared row-kind" ] || fail "A: link record is '$r'"

# ── B ──────────────────────────────────────────────────────────────────────
make_app b fwwin
( cd "$TMP/b" && "$MCPP" build > build.log 2>&1 ) || fail "B: build failed" "$TMP/b/build.log"
[ -z "$(find "$TMP/b/target" -name libfw.so | head -1)" ] || fail "B: a non-matching row built libfw.so"
r=$(reason_of "$(find "$TMP/b/target" -name resolution.json | head -1)" "demo.fw@0.1.0")
[ "$r" = "static default" ] || fail "B: link record is '$r'"

# ── C ──────────────────────────────────────────────────────────────────────
make_app c fwlinux ', linkage = "static"'
( cd "$TMP/c" && "$MCPP" build > build.log 2>&1 ) || fail "C: build failed" "$TMP/c/build.log"
grep -qF "its manifest states [target.'cfg(os = \"linux\")'.targets.fw] kind = \"shared\"" "$TMP/c/build.log" \
    || fail "C: the degradation does not name the row's statement" "$TMP/c/build.log"
rm -rf "$TMP/c/target"
if ( cd "$TMP/c" && "$MCPP" build --strict > strict.log 2>&1 ); then
    fail "C: --strict accepted the degradation" "$TMP/c/strict.log"
fi

# ── D ──────────────────────────────────────────────────────────────────────
make_fw nosuch linux '[target.'"'"'cfg(os = "linux")'"'"'.targets.nope]
kind = "shared"'
make_app d1 nosuch
if ( cd "$TMP/d1" && "$MCPP" build > build.log 2>&1 ); then fail "D: a row naming no target was accepted"; fi
grep -qF "names no target of this package" "$TMP/d1/build.log" || fail "D: refusal wording" "$TMP/d1/build.log"

mkdir -p "$TMP/prog/src"
cat > "$TMP/prog/mcpp.toml" <<'TOML'
[package]
name    = "prog"
version = "0.1.0"
[targets.prog]
kind = "bin"
main = "src/main.cpp"
[target.'cfg(os = "linux")'.targets.prog]
kind = "shared"
TOML
printf 'int main() { return 0; }\n' > "$TMP/prog/src/main.cpp"
if ( cd "$TMP/prog" && "$MCPP" build > build.log 2>&1 ); then fail "D: a row made a program shared"; fi
grep -qF "is a program target" "$TMP/prog/build.log" || fail "D: program refusal wording" "$TMP/prog/build.log"

cat > "$TMP/prog/mcpp.toml" <<'TOML'
[package]
name    = "prog"
version = "0.1.0"
[targets.lib]
kind = "lib"
[target.'cfg(os = "linux")'.targets.lib]
kind = "bin"
TOML
if ( cd "$TMP/prog" && "$MCPP" build > build.log 2>&1 ); then fail "D: a row kind 'bin' was accepted"; fi
grep -qF "a row chooses between the library forms only" "$TMP/prog/build.log" || fail "D: kind refusal wording" "$TMP/prog/build.log"

echo "OK"
