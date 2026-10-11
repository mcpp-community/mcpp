#!/usr/bin/env bash
# requires: elf
# 907 -- a library's interface has one definition (SPEC-008 v0.2, mcpp#797).
#
#   A  `[lib] path` naming no file fails the package's own build;
#   B  where the package is a dependency, the same statement is a warning
#      (its consumer cannot edit it), and the dependency's modules are still
#      imported by the names they declare;
#   C  `[modules] exports` is an assertion about the public modules: a public
#      module it does not list fails the package's own build, and a module
#      that is exported but not public is not one of them;
#   D  a consumer importing an exported module that is not public is warned
#      (W3), from build.mcpp as from the target.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/lib/src" "$TMP/app/src"
printf 'export module acme.lib;\nexport import acme.lib.part;\nexport int lib_value() { return 40; }\n' > "$TMP/lib/src/lib.cppm"
printf 'export module acme.lib.part;\nexport int part_value() { return 2; }\n' > "$TMP/lib/src/part.cppm"
printf 'export module acme.lib.detail;\nexport int detail_value() { return 0; }\n' > "$TMP/lib/src/detail.cppm"
cat > "$TMP/lib/mcpp.toml" <<'EOF'
[package]
namespace = "acme"
name      = "lib"
version   = "0.1.0"
[lib]
path = "src/missing.cppm"
[targets.lib]
kind = "lib"
EOF

# A
cd "$TMP/lib"
"$MCPP" build > a.log 2>&1 && fail "A: a [lib] path naming no file was accepted in the package's own build" a.log
grep -q "missing.cppm" a.log || fail "A: the error does not name the path" a.log

# B
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.acme]\nlib = { path = "../lib" }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app/mcpp.toml"
printf '#include <cstdio>\nimport acme.lib;\nint main() { std::printf("V=%%d\\n", lib_value() + part_value()); }\n' > "$TMP/app/src/main.cpp"
cd "$TMP/app"
"$MCPP" build > b.log 2>&1 || fail "B: a dependency's missing [lib] path failed its consumer" b.log
grep -q "names no file" b.log || fail "B: the consumer was not told" b.log
[ "$("$MCPP" run 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "B: value" b.log

# C
cd "$TMP/lib"
sed -i 's|path = "src/missing.cppm"|path = "src/lib.cppm"|' mcpp.toml
printf '[modules]\nexports = ["acme.lib"]\n' >> mcpp.toml
"$MCPP" build > c1.log 2>&1 && fail "C: a public module missing from [modules] exports was accepted" c1.log
grep -q "acme.lib.part" c1.log || fail "C: the error does not name the public module" c1.log
sed -i 's|exports = \["acme.lib"\]|exports = ["acme.lib", "acme.lib.part"]|' mcpp.toml
"$MCPP" build > c2.log 2>&1 || fail "C: exports listing the public modules, and not the private one, was refused" c2.log

# D
cd "$TMP/app"
printf '#include <cstdio>\nimport acme.lib;\nimport acme.lib.detail;\nint main() { std::printf("V=%%d\\n", lib_value() + part_value() + detail_value()); }\n' > src/main.cpp
"$MCPP" build > d.log 2>&1 || fail "D: the build failed" d.log
grep -q "acme.lib.detail" d.log && grep -qi "not one of that package's public modules" d.log \
    || fail "D: importing a module that is not public was not warned about" d.log
echo "PASS: 907_the_library_interface_is_stated_once"
