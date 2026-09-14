#!/usr/bin/env bash
# requires: elf
# 692 -- `linkage = "static" | "shared"` beside `kind` states a library's
# DEFAULT link form (#642 E1). `kind = "shared"` constrains a package to the
# shared form; `linkage = "shared"` makes it the answer a consumer gets when it
# asks for nothing, and an explicit consumer statement is honoured against it.
#
# Legs:
#   A. `[targets.fw] linkage = "shared"`, silent consumer: `libfw.so`, the
#      program NEEDs it, the record gives `shared package-default`, and
#      `--strict` accepts the build.
#   B. The consumer's edge says `linkage = "static"`: no `libfw.so`, one
#      information line names both statements, the record gives
#      `static requested`, and `--strict` accepts the build.
#   C. The consumer's `[build] dependency_linkage = "static"` also outranks the
#      package's default.
#   D. The row form: a matching row gives the shared default, a row that does
#      not match leaves the static default.
#   E. Replacement: a matching row `linkage = "static"` after an unconditional
#      `kind = "shared"` returns the package to a form the consumer chooses.
#   F. Refusals: `kind = "shared"` with `linkage` in one table, both keys in one
#      row, an unknown value, and `linkage` on a program target.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

record_of() {  # record_of <project dir> <canonical>
    python3 - "$(find "$1/target" -name resolution.json | head -1)" "$2" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
for p in doc["graph"]["packages"]:
    if p["package"]["canonical"] == sys.argv[2]:
        print(p.get("link", {}).get("form", ""), p.get("link", {}).get("reason", ""))
PY
}

make_fw() {  # make_fw <dir> <manifest tail>
    mkdir -p "$TMP/$1/src"
    printf '[package]\nnamespace = "demo"\nname      = "fw"\nversion   = "0.1.0"\n%s\n' "$2" \
        > "$TMP/$1/mcpp.toml"
    printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > "$TMP/$1/src/fw.cppm"
}
make_app() {  # make_app <dir> <fw dir> [edge options] [manifest tail]
    mkdir -p "$TMP/$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../%s"%s }\n%s\n' \
        "$1" "$2" "$3" "$4" > "$TMP/$1/mcpp.toml"
    printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$TMP/$1/src/main.cpp"
}
build() {  # build <dir> [flags] -- writes build.log
    ( cd "$TMP/$1" && rm -rf target && "$MCPP" build "${@:2}" > build.log 2>&1 )
}
libfw() { find "$TMP/$1/target" -name libfw.so | head -1; }

make_fw fwdefault '[targets.fw]
kind    = "lib"
linkage = "shared"'

# ── A ──────────────────────────────────────────────────────────────────────
make_app a fwdefault
build a --strict || fail "A: build --strict failed" "$TMP/a/build.log"
[ -n "$(libfw a)" ] || fail "A: no libfw.so for a silent consumer" "$TMP/a/build.log"
bin_a=$(find "$TMP/a/target" -type f -name a -path '*/bin/*' | head -1)
readelf -d "$bin_a" | grep -q 'NEEDED.*libfw\.so' || fail "A: the program does not NEED libfw.so"
r=$(record_of "$TMP/a" "demo.fw@0.1.0")
[ "$r" = "shared package-default" ] || fail "A: link record is '$r'"
"$bin_a" || fail "A: the program did not run"

# ── B ──────────────────────────────────────────────────────────────────────
make_app b fwdefault ', linkage = "static"'
build b --strict || fail "B: build --strict failed" "$TMP/b/build.log"
[ -z "$(libfw b)" ] || fail "B: an explicit static request still built libfw.so" "$TMP/b/build.log"
grep -qF "the consumer's \`linkage = \"static\"\` on this dependency overrides the package's default, [targets.fw] linkage = \"shared\"" \
    "$TMP/b/build.log" || fail "B: the information line does not name both statements" "$TMP/b/build.log"
grep -q "degraded\|warning: .*demo.fw" "$TMP/b/build.log" && fail "B: the override was reported as a degradation" "$TMP/b/build.log"
r=$(record_of "$TMP/b" "demo.fw@0.1.0")
[ "$r" = "static requested" ] || fail "B: link record is '$r'"

# ── C ──────────────────────────────────────────────────────────────────────
make_app c fwdefault '' '[build]
dependency_linkage = "static"'
build c || fail "C: build failed" "$TMP/c/build.log"
[ -z "$(libfw c)" ] || fail "C: dependency_linkage = static did not outrank the default" "$TMP/c/build.log"
r=$(record_of "$TMP/c" "demo.fw@0.1.0")
[ "$r" = "static requested" ] || fail "C: link record is '$r'"

# ── D ──────────────────────────────────────────────────────────────────────
make_fw fwrowlinux '[targets.fw]
kind = "lib"
[target.'"'"'cfg(os = "linux")'"'"'.targets.fw]
linkage = "shared"'
make_fw fwrowwin '[targets.fw]
kind = "lib"
[target.'"'"'cfg(os = "windows")'"'"'.targets.fw]
linkage = "shared"'
make_app d1 fwrowlinux
build d1 || fail "D: build failed (matching row)" "$TMP/d1/build.log"
[ -n "$(libfw d1)" ] || fail "D: a matching row did not give the shared default" "$TMP/d1/build.log"
r=$(record_of "$TMP/d1" "demo.fw@0.1.0")
[ "$r" = "shared package-default" ] || fail "D: matching row record is '$r'"
make_app d2 fwrowwin
build d2 || fail "D: build failed (non-matching row)" "$TMP/d2/build.log"
[ -z "$(libfw d2)" ] || fail "D: a non-matching row built libfw.so" "$TMP/d2/build.log"
r=$(record_of "$TMP/d2" "demo.fw@0.1.0")
[ "$r" = "static default" ] || fail "D: non-matching row record is '$r'"

# ── E ──────────────────────────────────────────────────────────────────────
make_fw fwreplace '[targets.fw]
kind = "shared"
[target.'"'"'cfg(os = "linux")'"'"'.targets.fw]
linkage = "static"'
make_app e fwreplace
build e --strict || fail "E: build --strict failed" "$TMP/e/build.log"
[ -z "$(libfw e)" ] || fail "E: the row default did not replace kind = shared" "$TMP/e/build.log"
r=$(record_of "$TMP/e" "demo.fw@0.1.0")
[ "$r" = "static package-default" ] || fail "E: link record is '$r'"

# ── F ──────────────────────────────────────────────────────────────────────
refused() {  # refused <leg> <fw manifest tail> <expected text>
    make_fw "fw$1" "$2"
    make_app "$1" "fw$1"
    if build "$1"; then fail "F($1): the manifest was accepted" "$TMP/$1/build.log"; fi
    grep -qF "$3" "$TMP/$1/build.log" || fail "F($1): refusal wording" "$TMP/$1/build.log"
}
refused f1 '[targets.fw]
kind    = "shared"
linkage = "static"' 'states both `kind = "shared"` and `linkage = "static"`'
refused f2 '[targets.fw]
kind = "lib"
[target.'"'"'cfg(os = "linux")'"'"'.targets.fw]
kind    = "shared"
linkage = "shared"' 'states both `kind` and `linkage`'
refused f3 '[targets.fw]
kind    = "lib"
linkage = "dynamic"' 'the default form of a library is `static` or `shared`'

mkdir -p "$TMP/prog/src"
cat > "$TMP/prog/mcpp.toml" <<'TOML'
[package]
name    = "prog"
version = "0.1.0"
[targets.prog]
kind = "bin"
main = "src/main.cpp"
[target.'cfg(os = "linux")'.targets.prog]
linkage = "shared"
TOML
printf 'int main() { return 0; }\n' > "$TMP/prog/src/main.cpp"
if build prog; then fail "F: a row gave a program target a default link form" "$TMP/prog/build.log"; fi
grep -qF "is a program target, which has no link form" "$TMP/prog/build.log" \
    || fail "F: program refusal wording" "$TMP/prog/build.log"

echo "OK"
