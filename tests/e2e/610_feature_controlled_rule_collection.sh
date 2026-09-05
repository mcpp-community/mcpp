#!/usr/bin/env bash
# requires: gcc
# 610_feature_controlled_rule_collection.sh — one package, several rules, the
# set chosen by the consumer's feature request (mcpp 2026.9.5.3+).
#
# Before this, a host-module package contributed exactly one module: the unit
# at its lib root. A collection such as `mcpp:plugins` -- `mcpp.rules.cuda`,
# `mcpp.rules.spirv`, later `mcpp.tools.*` -- therefore needed one package per
# member, and a consumer could not say "these two" in the one place it says
# everything else about a dependency. Now every module INTERFACE unit among the
# package's resolved sources is a host module of its own, and `[features.<f>]
# sources` is what puts a unit into that set.
#
# Pinned here:
#   1. a feature the consumer activates makes its unit importable, under the
#      name the unit declares, and that unit may import the lib root;
#   2. a unit whose feature is NOT activated is not compiled and cannot be
#      imported -- the module set is the feature set, not the file set;
#   3. two activated features give two modules;
#   4. a collection in the `mcpp` namespace draws no reserved-prefix warning,
#      and the same collection under another namespace draws one per unit;
#   5. a package with neither a lib root nor a listed interface unit keeps the
#      diagnostic it had.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

write_collection() {   # $1 = namespace
    rm -rf plugins
    mkdir -p plugins/src plugins/rules
    cat > plugins/mcpp.toml <<EOT
[package]
name      = "plugins"
namespace = "$1"
version   = "0.1.0"

[language]
standard = "c++23"

[build]
sources = ["src/plugins.cppm"]

[features]
rules-a = { sources = ["rules/a.cppm"] }
rules-b = { sources = ["rules/b.cppm"] }

[targets.plugins]
kind = "lib"
EOT
    cat > plugins/src/plugins.cppm <<'EOT'
export module mcpp.plugins;
import std;
export namespace mcpp::plugins {
inline constexpr std::string_view version = "0.1.0";
}
EOT
    cat > plugins/rules/a.cppm <<'EOT'
export module mcpp.rules.a;
import std;
import mcpp;
import mcpp.plugins;   // the lib root precedes every feature unit
export namespace mcpp::rules::a {
inline void apply() {
    mcpp::cxxflag("-DRULE_A=1");
    // Unquoted on purpose: the token is stringized by the consumer, and a
    // quoted value would have to survive two layers of shell quoting.
    mcpp::cxxflag(std::format("-DPLUGINS_VERSION={}", mcpp::plugins::version).c_str());
}
}
EOT
    cat > plugins/rules/b.cppm <<'EOT'
export module mcpp.rules.b;
import std;
import mcpp;
export namespace mcpp::rules::b {
inline void apply() { mcpp::cxxflag("-DRULE_B=1"); }
}
EOT
}

write_app() {   # $1 = namespace, $2 = features (TOML list body), $3 = imports (space-separated module names)
    rm -rf app
    mkdir -p app/src
    cat > app/mcpp.toml <<EOT
[package]
name    = "app"
version = "0.1.0"

[language]
standard = "c++23"

[dependencies.$1]
plugins = { path = "../plugins", features = [$2], host-module = true }
EOT
    {
        echo 'import std;'
        echo 'import mcpp;'
        for m in $3; do echo "import $m;"; done
        echo 'int main() {'
        for m in $3; do echo "    ${m//./::}::apply();"; done
        echo '}'
    } > app/build.mcpp
    cat > app/src/main.cpp <<'EOT'
#include <cstdio>
#ifndef RULE_A
#define RULE_A 0
#endif
#ifndef RULE_B
#define RULE_B 0
#endif
#ifndef PLUGINS_VERSION
#define PLUGINS_VERSION none
#endif
#define STR_(x) #x
#define STR(x) STR_(x)
int main() { std::printf("A=%d B=%d V=%s\n", RULE_A, RULE_B, STR(PLUGINS_VERSION)); }
EOT
}

# ── 1. one feature, one module, and the unit imports the lib root ───────────
write_collection mcpp
write_app mcpp '"rules-a"' 'mcpp.rules.a'
cd app
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: a feature-selected rule did not build"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^A=' | tail -1)"
[[ "$out" == 'A=1 B=0 V=0.1.0' ]] || { echo "FAIL: expected A=1 B=0 V=0.1.0, got: $out"; exit 1; }
echo "PASS: a feature the consumer activates makes its unit importable under its declared name"
if grep -q 'prefix is reserved for rules maintained by the mcpp project' b1.log; then
    echo "FAIL: the mcpp namespace drew a reserved-prefix warning"; cat b1.log; exit 1; fi
echo "PASS: a collection in the mcpp namespace draws no reserved-prefix warning"
cd ..

# ── 2. a unit whose feature is not activated is not importable ───────────────
write_app mcpp '"rules-a"' 'mcpp.rules.b'
cd app
if "$MCPP" build > b2.log 2>&1; then
    echo "FAIL: a rule whose feature is not active was importable"; cat b2.log; exit 1; fi
grep -q 'mcpp.rules.b' b2.log || { echo "FAIL: the refusal does not name the module"; cat b2.log; exit 1; }
echo "PASS: a unit whose feature is not activated is not compiled and cannot be imported"
cd ..

# ── 3. two features, two modules ─────────────────────────────────────────────
write_app mcpp '"rules-a", "rules-b"' 'mcpp.rules.a mcpp.rules.b'
cd app
"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: two feature-selected rules did not build"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^A=' | tail -1)"
[[ "$out" == 'A=1 B=1 V=0.1.0' ]] || { echo "FAIL: expected A=1 B=1 V=0.1.0, got: $out"; exit 1; }
echo "PASS: two activated features give two modules"
cd ..

# ── 4. the control: another namespace is warned about, once per unit ─────────
write_collection acme
write_app acme '"rules-a", "rules-b"' 'mcpp.rules.a mcpp.rules.b'
cd app
"$MCPP" build > b4.log 2>&1 || { cat b4.log; echo "FAIL: the acme collection did not build"; exit 1; }
n=$(grep -c 'prefix is reserved for rules maintained by the mcpp project' b4.log || true)
# three units -- the lib root `mcpp.plugins` and the two rules -- each claims the prefix
[[ "$n" -eq 3 ]] || { echo "FAIL: expected 3 reserved-prefix warnings (one per unit), got $n"; cat b4.log; exit 1; }
echo "PASS: the same collection under another namespace draws one warning per unit"
cd ..

# ── 5. neither a lib root nor a listed unit: the diagnostic is unchanged ─────
rm -rf plugins app
mkdir -p plugins/rules app/src
cat > plugins/mcpp.toml <<'EOT'
[package]
name    = "plugins"
version = "0.1.0"

[build]
sources = ["rules/*.cppm"]

[targets.plugins]
kind = "lib"
EOT
cat > plugins/rules/impl.cppm <<'EOT'
module nothing.exported;   // an implementation unit is not an interface
EOT
cat > app/mcpp.toml <<'EOT'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
plugins = { path = "../plugins", host-module = true }
EOT
echo 'import mcpp; int main() {}' > app/build.mcpp
echo 'int main() {}' > app/src/main.cpp
cd app
if "$MCPP" build > b5.log 2>&1; then
    echo "FAIL: a host-module package with no interface unit built"; cat b5.log; exit 1; fi
grep -q 'no interface unit at' b5.log || { echo "FAIL: the missing-lib-root diagnostic changed"; cat b5.log; exit 1; }
echo "PASS: a package with neither a lib root nor a listed interface unit keeps its diagnostic"

echo "PASS: feature-controlled rule collection"
