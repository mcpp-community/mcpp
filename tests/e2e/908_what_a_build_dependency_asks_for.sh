#!/usr/bin/env bash
# requires: elf
# 908 -- what a [build-dependencies] entry asks for (SPEC-004 §12 item 5).
#
#   A  with neither `tools` nor `host-module`, the library: build.mcpp imports it;
#   B  with only `tools`, the program and no library: an import of the library
#      is refused by name, before the compiler, pointing at `host-module`;
#   C  `host-module = true` beside `tools` gives both;
#   D  `host-module = false` declines the library.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/gen/src" "$TMP/gen/tools" "$TMP/app/src"
printf 'export module acme.gen;\nexport int gen_value() { return 42; }\n' > "$TMP/gen/src/gen.cppm"
printf '#include <cstdio>\nint main() { std::puts("generated"); }\n' > "$TMP/gen/tools/gen_main.cpp"
cat > "$TMP/gen/mcpp.toml" <<'EOF'
[package]
namespace = "acme"
name      = "gen"
version   = "0.1.0"
[build]
sources = ["src/gen.cppm"]
[targets.gen]
kind = "lib"
[targets.gentool]
kind = "bin"
main = "tools/gen_main.cpp"
EOF
printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > "$TMP/app/src/main.cpp"
printf 'import std;\nimport mcpp;\nimport acme.gen;\nint main() { mcpp::cxxflag(std::format("-DV={}", gen_value()).c_str()); }\n' > "$TMP/app/build.mcpp"
manifest() { printf '[package]\nname = "app"\nversion = "0.1.0"\n[build-dependencies.acme]\ngen = { path = "../gen"%s }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' "$1" > "$TMP/app/mcpp.toml"; }
cd "$TMP/app"

manifest ""
"$MCPP" build > a.log 2>&1 || fail "A: the default entry did not give the library" a.log

manifest ', tools = ["gentool"]'
"$MCPP" build > b.log 2>&1 && fail "B: a tools-only entry gave the library" b.log
grep -q "imports 'acme.gen'" b.log || fail "B: the refusal does not name the module" b.log
grep -q "host-module = true" b.log || fail "B: the refusal does not name the key" b.log
grep -qi "failed to read compiled module" b.log && fail "B: the compiler was reached" b.log

manifest ', tools = ["gentool"], host-module = true'
"$MCPP" build > c.log 2>&1 || fail "C: tools and host-module = true did not give both" c.log

manifest ', host-module = false'
"$MCPP" build > d.log 2>&1 && fail "D: host-module = false still gave the library" d.log
grep -q "imports 'acme.gen'" d.log || fail "D: the refusal does not name the module" d.log
echo "PASS: 908_what_a_build_dependency_asks_for"
