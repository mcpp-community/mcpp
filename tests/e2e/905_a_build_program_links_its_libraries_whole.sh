#!/usr/bin/env bash
# requires: elf
# 905 -- a library a build program uses is compiled and linked whole
# (mcpp#797, #797 Q4/Q5): its implementation units, its C sources and its own
# dependencies reach the build program. The single-unit path compiled one
# object per interface unit and nothing else, so a library whose functions are
# defined out of line could not be used at all.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/base/src" "$TMP/lib/src" "$TMP/app/src"
printf 'export module acme.base;\nexport int base_two();\n' > "$TMP/base/src/base.cppm"
printf 'module acme.base;\nint base_two() { return 2; }\n' > "$TMP/base/src/base_impl.cpp"
printf '[package]\nname = "base"\nversion = "0.1.0"\n[targets.base]\nkind = "lib"\n' > "$TMP/base/mcpp.toml"
printf 'export module lib;\nexport int lib_value();\n' > "$TMP/lib/src/lib.cppm"
cat > "$TMP/lib/src/lib_impl.cpp" <<'EOF'
module lib;
import acme.base;
extern "C" int c_forty();
int lib_value() { return c_forty() + base_two(); }
EOF
printf 'int c_forty(void) { return 40; }\n' > "$TMP/lib/src/forty.c"
printf '[package]\nname = "lib"\nversion = "0.1.0"\n[dependencies]\nbase = { path = "../base" }\n[targets.lib]\nkind = "lib"\n' > "$TMP/lib/mcpp.toml"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[build-dependencies]\nlib = { path = "../lib" }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app/mcpp.toml"
printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > "$TMP/app/src/main.cpp"
printf 'import std;\nimport mcpp;\nimport lib;\nint main() { mcpp::cxxflag(std::format("-DV={}", lib_value()).c_str()); }\n' > "$TMP/app/build.mcpp"

cd "$TMP/app"
"$MCPP" build > b.log 2>&1 || fail "a library with implementation units, C sources and a dependency did not link into the build program" b.log
[ "$("$MCPP" run 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "the program computed another value" b.log
echo "PASS: 905_a_build_program_links_its_libraries_whole"
