#!/usr/bin/env bash
# requires: elf
# 906 -- a build program is compiled again exactly when its identity moves, and
# a build whose programs stand does not plan their graph (mcpp#797, R6).
#
#   A  an edit to a header a path library includes reruns the program with the
#      new value (the identity used to hash the interface file only);
#   B  a build in which nothing changed states that the identity is current
#      and plans no build programs' graph;
#   C  the identity and the reason are in the verbose log.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/hl/src" "$TMP/app/src"
printf '#define HL_VALUE 42\n' > "$TMP/hl/src/value.h"
printf 'module;\n#include "value.h"\nexport module hl;\nexport int hl_value() { return HL_VALUE; }\n' > "$TMP/hl/src/hl.cppm"
printf '[package]\nname = "hl"\nversion = "0.1.0"\n[targets.hl]\nkind = "lib"\n' > "$TMP/hl/mcpp.toml"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[build-dependencies]\nhl = { path = "../hl" }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app/mcpp.toml"
printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > "$TMP/app/src/main.cpp"
printf 'import std;\nimport mcpp;\nimport hl;\nint main() { mcpp::cxxflag(std::format("-DV={}", hl_value()).c_str()); }\n' > "$TMP/app/build.mcpp"

cd "$TMP/app"
"$MCPP" build > b1.log 2>&1 || fail "the first build failed" b1.log
[ "$("$MCPP" run 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "first value" b1.log

# B, C: nothing changed. A source the fast path sweeps is touched so that
# planning runs; the program's identity stands.
sleep 1
touch src/main.cpp
"$MCPP" build --verbose > b2.log 2>&1 || fail "B: the second build failed" b2.log
grep -qE "build-program: app: identity [0-9a-f]+ current" b2.log || fail "C: the log does not state the identity stood" b2.log
grep -q "host-graph: build programs in" b2.log && fail "B: the build programs' graph was planned although nothing changed" b2.log

# A
sleep 1
printf '#define HL_VALUE 43\n' > "$TMP/hl/src/value.h"
"$MCPP" build --verbose > b3.log 2>&1 || fail "A: the build after the header edit failed" b3.log
grep -qE "build-program: app: identity [0-9a-f]+ -> [0-9a-f]+, compiled" b3.log || fail "C: the log does not state why the program was compiled" b3.log
[ "$("$MCPP" run 2>/dev/null | grep '^V=' | tail -1)" = "V=43" ] || fail "A: the program ran on the old header" b3.log
echo "PASS: 906_a_build_program_is_compiled_when_its_graph_changes"
