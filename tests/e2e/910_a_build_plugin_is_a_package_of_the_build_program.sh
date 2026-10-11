#!/usr/bin/env bash
# requires: elf
# 910 -- build plugins (SPEC-007 §10, mcpp#797). A unit that imports
# `mcpp.core` (or `mcpp`) is a build-plugin unit; nothing else declares it.
#
#   A  a plugin built on another plugin and on an ordinary library, each named
#      in its [dependencies], serves a consumer's build program (S1, S2);
#   B  P3: the plugin builds as the root of a build, in the build-program
#      configuration, and its tests run;
#   C  P3: `--target` for another machine is refused, naming the plugin;
#   D  P1: a plugin reached through [dependencies] of a target is refused,
#      naming the chain and [build-dependencies];
#   E  P4: a program target importing mcpp is refused;
#   F  P5: `mcpp pack` refuses a plugin;
#   G  a library a plugin depends on is importable by the plugin's consumers'
#      build programs when the plugin hands it on (`reexport = true` on its
#      [dependencies] edge), and refused, naming `reexport`, when it does not.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

lib() {  # $1 = name, $2 = dependencies table body
    mkdir -p "$TMP/$1/src"
    printf '[package]\nnamespace = "acme"\nname = "%s"\nversion = "0.1.0"\n[dependencies.acme]\n%s\n[targets.%s]\nkind = "lib"\n' \
        "$1" "$2" "$1" > "$TMP/$1/mcpp.toml"
}
lib util ""
printf 'export module acme.util;\nexport int util_base() { return 40; }\n' > "$TMP/util/src/util.cppm"
lib core-rule ""
cat > "$TMP/core-rule/src/core-rule.cppm" <<'EOF'
export module acme.core_rule;
import std;
import mcpp;
export namespace acme::core_rule {
inline void define(std::string_view key, int value) { mcpp::define(std::format("{}={}", key, value).c_str()); }
}
EOF
lib rule 'core-rule = { path = "../core-rule" }
util = { path = "../util" }'
cat > "$TMP/rule/src/rule.cppm" <<'EOF'
export module acme.rule;
import mcpp;
import acme.core_rule;
import acme.util;
export namespace acme::rule {
inline int value() { return util_base() + 2; }
inline void go() { acme::core_rule::define("V", value()); }
}
EOF

# A
mkdir -p "$TMP/app/src"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[build-dependencies.acme]\nrule = { path = "../rule" }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app/mcpp.toml"
printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > "$TMP/app/src/main.cpp"
printf 'import acme.rule;\nint main() { acme::rule::go(); }\n' > "$TMP/app/build.mcpp"
cd "$TMP/app"
"$MCPP" build > a.log 2>&1 || fail "A: a plugin built on a plugin and a library did not serve the build program" a.log
[ "$("$MCPP" run 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "A: value" a.log
if find target -path '*/obj/*' -name '*.m.o' -not -path '*/.build-mcpp/*' | grep -E 'rule|util' | grep -q .; then
    fail "A: a package of the build program reached the target"
fi

# B
mkdir -p "$TMP/rule/tests"
printf 'import mcpp;\nimport acme.rule;\nint main() { return acme::rule::value() == 42 ? 0 : 1; }\n' > "$TMP/rule/tests/value.cpp"
cd "$TMP/rule"
"$MCPP" build > b1.log 2>&1 || fail "B: the plugin did not build as the root of a build" b1.log
grep -q "build-program" b1.log || fail "B: the build does not state its configuration" b1.log
"$MCPP" test > b2.log 2>&1 || fail "B: the plugin's tests did not run" b2.log
grep -q "1 passed" b2.log || fail "B: the test did not pass" b2.log

# C
"$MCPP" build --target aarch64-linux-gnu > c.log 2>&1 && fail "C: --target for another machine was accepted" c.log
grep -q "acme.rule' is a build plugin" c.log && grep -q "cannot apply" c.log \
    || fail "C: the refusal does not name the plugin" c.log

# D
mkdir -p "$TMP/app2/src"
printf '[package]\nname = "app2"\nversion = "0.1.0"\n[dependencies.acme]\nrule = { path = "../rule" }\n[targets.app2]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app2/mcpp.toml"
printf 'int main() { return 0; }\n' > "$TMP/app2/src/main.cpp"
cd "$TMP/app2"
"$MCPP" build > d.log 2>&1 && fail "D: a plugin in [dependencies] of a target was accepted" d.log
grep -q "is a build plugin" d.log && grep -q "app2 → acme.rule" d.log && grep -q "\[build-dependencies\]" d.log \
    || fail "D: the refusal does not name the chain and the table" d.log

# E
mkdir -p "$TMP/app3/src"
printf '[package]\nname = "app3"\nversion = "0.1.0"\n[targets.app3]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app3/mcpp.toml"
printf 'import mcpp;\nint main() { return 0; }\n' > "$TMP/app3/src/main.cpp"
cd "$TMP/app3"
"$MCPP" build > e.log 2>&1 && fail "E: a program importing mcpp was accepted" e.log
grep -q "exists only in a build" e.log || fail "E: the refusal does not say why" e.log

# F
cd "$TMP/rule"
"$MCPP" pack > f.log 2>&1 && fail "F: mcpp pack accepted a plugin" f.log
grep -q "is a build plugin" f.log || fail "F: the refusal does not say why" f.log
# G
cd "$TMP/app"
printf 'import acme.rule;\nimport acme.util;\nint main() { if (util_base() == 40) acme::rule::go(); }\n' > build.mcpp
"$MCPP" build > g1.log 2>&1 && fail "G: a library the plugin does not hand on was importable" g1.log
grep -q "reexport = true" g1.log || fail "G: the refusal does not name reexport" g1.log
sed -i 's|util = { path = "../util" }|util = { path = "../util", reexport = true }|' "$TMP/rule/mcpp.toml"
"$MCPP" build > g2.log 2>&1 || fail "G: a library the plugin hands on was refused" g2.log
[ "$("$MCPP" run 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "G: value" g2.log
echo "PASS: 910_a_build_plugin_is_a_package_of_the_build_program"
