#!/usr/bin/env bash
# requires: elf
# 909 -- the deprecation registry (docs/51 §Deprecations, mcpp#797).
#
#   A  `[dependencies] x = { host-module = true }` keeps its meaning -- the
#      package serves the build program and not the target -- and is warned
#      about with the release that deprecated it and the one that removes it;
#   B  `mcpp why compat` lists the use, and `--format json` states its row;
#   C  `--strict` refuses it;
#   D  a plugin whose library imports a package from its own
#      [build-dependencies] (`host-module = true`) keeps building, and is told.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/inner/src" "$TMP/rule/src" "$TMP/app/src"
printf 'export module inner;\nexport int inner_value() { return 40; }\n' > "$TMP/inner/src/inner.cppm"
printf '[package]\nname = "inner"\nversion = "0.1.0"\n[targets.inner]\nkind = "lib"\n' > "$TMP/inner/mcpp.toml"
printf 'export module rule;\nimport std;\nimport mcpp;\nimport inner;\nexport namespace rule { inline void go() { mcpp::cxxflag(std::format("-DV={}", inner_value() + 2).c_str()); } }\n' > "$TMP/rule/src/rule.cppm"
printf '[package]\nname = "rule"\nversion = "0.1.0"\n[build-dependencies]\ninner = { path = "../inner", host-module = true }\n[targets.rule]\nkind = "lib"\n' > "$TMP/rule/mcpp.toml"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\nrule = { path = "../rule", host-module = true }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app/mcpp.toml"
printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > "$TMP/app/src/main.cpp"
printf 'import mcpp;\nimport rule;\nint main() { rule::go(); }\n' > "$TMP/app/build.mcpp"

cd "$TMP/app"
"$MCPP" build > a.log 2>&1 || fail "A/D: the deprecated spellings stopped building" a.log
[ "$("$MCPP" run 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "A/D: value" a.log
grep -q "deprecated since mcpp 2026.10.11.1 and removed from mcpp 2027.1" a.log || fail "A: the warning does not state both releases" a.log
grep -q "\[build-dependencies\]" a.log || fail "A: the warning does not name the canonical spelling" a.log
grep -q "plugin-imports-from-build-dependencies\|\[build-dependencies\] inner" a.log || fail "D: the plugin's reading was not reported" a.log
if find target -path '*/obj/*' -name 'rule.m.o' -not -path '*/.build-mcpp/*' | grep -q .; then
    fail "A: the package reached the target"
fi

"$MCPP" why compat > why.log 2>&1 || fail "B: mcpp why compat failed" why.log
grep -q "host-module-in-dependencies" why.log || fail "B: the use is not listed" why.log
"$MCPP" why compat --format json > why.json 2>/dev/null || fail "B: --format json failed"
grep -q '"floor": "2027.1"' why.json || fail "B: the JSON does not state the floor" why.json

"$MCPP" build --strict > s.log 2>&1 && fail "C: --strict accepted a deprecated reading" s.log
grep -q "refuses the deprecated readings" s.log || fail "C: the refusal does not say why" s.log
echo "PASS: 909_deprecated_readings_are_registered_and_kept"
