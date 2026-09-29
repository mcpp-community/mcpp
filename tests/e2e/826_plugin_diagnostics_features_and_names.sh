#!/usr/bin/env bash
# requires: unix-shell
# 826_plugin_diagnostics_features_and_names.sh -- #734 E11, E7 and E10.
#
# A third-party build plugin, `acme.gen`, offers `mcpp.acme.gen` from its lib
# root and two modules behind features. A consumer's build program uses it.
#
#   D1  a structured diagnostic is rendered in the engine's form, with its
#       impact and hint lines (E11);
#   D2  after a source changes, the next build is a cache hit that does not
#       run the program, and it reports the diagnostic again (E11);
#   N1  importing a module that sits behind a feature the edge did not enable
#       fails naming the package and the feature (E7);
#   R1  `mcpp.acme.*` from namespace `acme` draws no naming warning (E10);
#   R2  `mcpp.rules.*` from namespace `acme` is warned about, and the warning
#       names the accepted form `mcpp.acme.*` (E10).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

mkdir -p gen/src app/src
cat > gen/mcpp.toml <<'EOF'
[package]
namespace = "acme"
name      = "gen"
version   = "0.1.0"

[lib]
path = "src/gen.cppm"

[features.extra]
sources = ["src/extra.cppm"]

[features.bad]
sources = ["src/bad.cppm"]
EOF
cat > gen/src/gen.cppm <<'EOF'
export module mcpp.acme.gen;
import mcpp.core;
export namespace acme {
inline void tell() {
    mcpp::report({.severity = "warning",
                  .message  = "the generator found no schema",
                  .impact   = "no bindings are generated",
                  .hint     = "add schema/*.proto"});
}
}
EOF
printf 'export module mcpp.acme.gen.extra;\nexport namespace acme { inline int extra() { return 1; } }\n' > gen/src/extra.cppm
printf 'export module mcpp.rules.bad;\nexport namespace acme { inline int bad() { return 2; } }\n' > gen/src/bad.cppm

cd app
printf 'int main() { return 0; }\n' > src/main.cpp
edge() { printf '[package]\nname = "app826"\nversion = "0.1.0"\n\n[build-dependencies]\n"acme.gen" = { path = "../gen", host-module = true%s }\n' "$1" > mcpp.toml; }

# D1, R1
edge ''
printf 'import mcpp.core;\nimport mcpp.acme.gen;\nint main() { acme::tell(); return 0; }\n' > build.mcpp
"$MCPP" build > d1.log 2>&1 || fail "D1: the build failed" d1.log
grep -q "the generator found no schema" d1.log || fail "D1: the message is missing" d1.log
grep -q "impact: no bindings are generated" d1.log || fail "D1: the impact line is missing" d1.log
grep -q "hint: add schema/\*.proto" d1.log || fail "D1: the hint line is missing" d1.log
grep -q "mcpp.acme.gen'.*mcpp\.\|claims an origin" d1.log && fail "R1: an own-namespace module was warned about" d1.log

# D2. `touch` first, as e2e 139 does: an unmodified build takes the project
# fast path and reaches no build program, so it would measure neither path.
touch src/main.cpp
# A reused program is stated under --verbose only (build output design revision 3, §7.1).
"$MCPP" build -v > d2.log 2>&1 || fail "D2: the second build failed" d2.log
grep -qE "^ *build\.mcpp .* cached" d2.log || fail "D2: the second build ran the program; the replay is not measured" d2.log
grep -q "impact: no bindings are generated" d2.log || fail "D2: the cached run did not report the diagnostic" d2.log

# N1
printf 'import mcpp.core;\nimport mcpp.acme.gen.extra;\nint main() { return acme::extra() == 1 ? 0 : 1; }\n' > build.mcpp
if "$MCPP" build > n1.log 2>&1; then fail "N1: a module behind a disabled feature was importable" n1.log; fi
grep -q "provided by: acme.gen, feature \"extra\" (not enabled)" n1.log || fail "N1: the error does not name the feature" n1.log
edge ', features = ["extra"]'
"$MCPP" build > n1b.log 2>&1 || fail "N1: enabling the named feature did not fix the build" n1b.log

# R2
edge ', features = ["bad"]'
printf 'import mcpp.core;\nimport mcpp.rules.bad;\nint main() { return acme::bad() == 2 ? 0 : 1; }\n' > build.mcpp
"$MCPP" build > r2.log 2>&1 || fail "R2: the build failed" r2.log
grep -q "uses 'mcpp.rules'" r2.log || fail "R2: a reserved segment was not warned about" r2.log
grep -q "'mcpp.acme.\*'" r2.log || fail "R2: the warning does not name the accepted form" r2.log

echo "OK"
