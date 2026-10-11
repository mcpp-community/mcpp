#!/usr/bin/env bash
# requires: elf python3
# 903 -- one package in [dependencies] and [build-dependencies] has two
# instances, each correct (mcpp#797, R2, R3; design §3.5, M1-M3, M5-M7, M9, M10).
#
# The two declarations used to merge into one edge that carried the
# build-program request, and the target-reachability test read that request as
# "build time only": the package's sources were cleared and the target's
# `import` failed with `failed to read compiled module`.
#
#   M1  same consumer, both tables: the target and build.mcpp both import it;
#   M2  `host-module = true` written beside it changes nothing;
#   M3  a feature requested only by the build dependency reaches the
#       build-program instance and not the target instance;
#   M5  a workspace: one member links it, another's build program uses it;
#   M6  the target reaches it through another library, build.mcpp directly;
#   M7  a dependency's own build.mcpp uses it while the root links it;
#   M9  the build database has a target set and a `:host` set for it, and
#       compile_commands.json describes the target units only;
#   M10 an edit to its header recompiles both instances and reruns the program.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/hl/include/hl" "$TMP/hl/src"
cat > "$TMP/hl/include/hl/hl.hpp" <<'EOF'
#pragma once
namespace hl {
inline int answer() { return 42; }
inline int x_on() {
#ifdef HL_X
    return 1;
#else
    return 0;
#endif
}
}
EOF
cat > "$TMP/hl/src/hl.cppm" <<'EOF'
module;
#include <hl/hl.hpp>
export module hl;
export namespace hl { using hl::answer; using hl::x_on; }
EOF
cat > "$TMP/hl/mcpp.toml" <<'EOF'
[package]
name    = "hl"
version = "0.1.0"
[build]
sources      = ["src/hl.cppm"]
include_dirs = ["include"]
[features]
x = { defines = ["HL_X"] }
[targets.hl]
kind = "lib"
EOF
program='import std;
import mcpp;
import hl;
int main() {
    mcpp::cxxflag(std::format("-DV={}", hl::answer()).c_str());
    mcpp::cxxflag(std::format("-DX_IN_PROGRAM={}", hl::x_on()).c_str());
}'
target_main='#include <cstdio>
import hl;
int main() { std::printf("V=%d target=%d xprog=%d xtarget=%d\n", V, hl::answer(), X_IN_PROGRAM, hl::x_on()); }'

app() {  # $1 dir, $2 extra manifest
    mkdir -p "$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n%s\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' \
        "$(basename "$1")" "$2" "$(basename "$1")" > "$1/mcpp.toml"
    printf '%s\n' "$target_main" > "$1/src/main.cpp"
    printf '%s\n' "$program" > "$1/build.mcpp"
}
run_line() { "$MCPP" run 2>/dev/null | grep '^V=' | tail -1; }

# M1, M3
app "$TMP/m1" $'[dependencies]\nhl = { path = "../hl" }\n[build-dependencies]\nhl = { path = "../hl", features = ["x"] }'
cd "$TMP/m1"
"$MCPP" build > b.log 2>&1 || fail "M1: one package in both tables did not build" b.log
[ "$(run_line)" = "V=42 target=42 xprog=1 xtarget=0" ] || fail "M1/M3: $(run_line)" b.log

# M2
sed -i 's/features = \["x"\] }/features = ["x"], host-module = true }/' mcpp.toml
"$MCPP" build > b2.log 2>&1 || fail "M2: host-module = true beside the entry broke the build" b2.log
grep -q "deprecated" b2.log && fail "M2: stating the default was reported" b2.log
[ "$(run_line)" = "V=42 target=42 xprog=1 xtarget=0" ] || fail "M2: $(run_line)" b2.log

# M9
"$MCPP" emit build-database --format json > db.json 2>/dev/null || fail "M9: emit build-database failed"
python3 - db.json <<'PY' || fail "M9: the build database does not describe both instances" db.json
import json, sys
doc = json.load(open(sys.argv[1]))["data"]["database"]
sets = {s["name"]: s for s in doc["sets"]}
assert "hl" in sets or any(n.endswith(".hl") for n in sets), sets.keys()
host = [n for n in sets if n.endswith("hl:host")]
assert host, sets.keys()
assert sets[host[0]]["ide"]["configuration"] == "build-program"
assert any(n.endswith(":build") for n in sets), sets.keys()
PY
"$MCPP" emit build-database --spec compile-commands > cc.json 2>/dev/null
grep -q 'build.mcpp' cc.json && fail "M9: compile_commands describes build.mcpp" cc.json

# M10
sleep 1
sed -i 's/return 42;/return 43;/' "$TMP/hl/include/hl/hl.hpp"
"$MCPP" build > b3.log 2>&1 || fail "M10: the build after a header edit failed" b3.log
[ "$(run_line)" = "V=43 target=43 xprog=1 xtarget=0" ] || fail "M10: an instance kept the old header: $(run_line)" b3.log
sed -i 's/return 43;/return 42;/' "$TMP/hl/include/hl/hl.hpp"

# M5
mkdir -p "$TMP/ws"
printf '[workspace]\nmembers = ["a", "b"]\n' > "$TMP/ws/mcpp.toml"
app "$TMP/ws/a" $'[dependencies]\nhl = { path = "../../hl" }'
printf '#include <cstdio>\nimport hl;\nint main() { std::printf("V=%%d\\n", hl::answer()); }\n' > "$TMP/ws/a/src/main.cpp"
rm "$TMP/ws/a/build.mcpp"
app "$TMP/ws/b" $'[build-dependencies]\nhl = { path = "../../hl" }'
printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > "$TMP/ws/b/src/main.cpp"
cd "$TMP/ws"
"$MCPP" build --workspace > w.log 2>&1 || fail "M5: the workspace did not build" w.log
[ "$("$MCPP" run -p a 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "M5: member a" w.log
[ "$("$MCPP" run -p b 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "M5: member b" w.log

# M6, M7
mkdir -p "$TMP/mid/src"
printf '[package]\nname = "mid"\nversion = "0.1.0"\n[dependencies]\nhl = { path = "../hl" }\n[build-dependencies]\nhl = { path = "../hl" }\n[targets.mid]\nkind = "lib"\n' > "$TMP/mid/mcpp.toml"
printf 'export module mid;\nimport hl;\nexport int via_mid() { return hl::answer() + MID; }\n' > "$TMP/mid/src/mid.cppm"
printf 'import std;\nimport mcpp;\nimport hl;\nint main() { mcpp::cxxflag(std::format("-DMID={}", hl::answer() - 42).c_str()); }\n' > "$TMP/mid/build.mcpp"
app "$TMP/m6" $'[dependencies]\nmid = { path = "../mid" }\n[build-dependencies]\nhl = { path = "../hl" }'
printf '#include <cstdio>\nimport mid;\nint main() { std::printf("V=%%d mid=%%d\\n", V, via_mid()); }\n' > "$TMP/m6/src/main.cpp"
cd "$TMP/m6"
"$MCPP" build > b.log 2>&1 || fail "M6/M7: a library reached through another one, used by two programs, did not build" b.log
[ "$(run_line)" = "V=42 mid=42" ] || fail "M6/M7: $(run_line)" b.log

echo "PASS: 903_one_package_in_both_tables_has_two_instances"
