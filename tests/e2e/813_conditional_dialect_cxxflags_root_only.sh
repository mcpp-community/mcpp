#!/usr/bin/env bash
# requires: gcc
# 813_conditional_dialect_cxxflags_root_only.sh -- #717, design 2026-09-27 §6:
# `[target.<selector>.build] dialect_cxxflags` is a graph-wide switch under a
# target condition. Before the fix (measured on 2026.9.27.1), the key inside
# `[target.linux.build]` was reported as "unsupported key 'dialect_cxxflags'
# (ignored)" and reached no command at all.
#
# Four properties, each with its own scenario below:
#
#   1. a matching row reaches the std BMI prebuild, the module scan and every
#      translation unit, in the order [build] (root, unconditional) then the
#      matching [target.<selector>.build] (§6.2: "entries are appended");
#   2. a selector that does not match the resolved target contributes nothing;
#   3. switching a manifest between a matching and a non-matching row rebuilds
#      the std BMI, and switching back reuses the earlier std-module cache
#      entry rather than rebuilding it a second time (A, then B, then A);
#   4. a DEPENDENCY's own `dialect_cxxflags` (conditional or not) reaches no
#      command, and toggling it does not change the OUTPUT DIRECTORY
#      fingerprint of the graph it belongs to (the graph-wide keys must be
#      excluded from a package's own per-package fingerprint contribution,
#      design §6.2 finding 7).
set -e
# The project-root copy of the compile database is opt-in since
# 2026.10.26.1 ([build] compile_commands); this test reads it.
export MCPP_COMPILE_COMMANDS=compile_commands.json

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

std_module_json_for() {   # $1 = a substring the std_flag field must contain
    grep -rl -- "$1" "$MCPP_HOME/build-cache/v1/std" 2>/dev/null | head -1
}

echo "== 813/1: a matching row reaches the std BMI, the scan and every TU, after the root's own [build] =="

mkdir -p "$TMP/one/src"
cd "$TMP/one"
cat > mcpp.toml <<'TOML'
[package]
name     = "app717a"
version  = "0.1.0"
standard = "c++23"

[build]
dialect_cxxflags = ["-DBASE717"]

[targets.app717a]
kind = "bin"
main = "src/main.cpp"

[target.linux.build]
dialect_cxxflags = ["-DX717A"]
TOML
printf 'import std;\nint main() { std::println("ok"); return 0; }\n' > src/main.cpp

"$MCPP" build --toolchain gcc@16.1.0 > build1.log 2>&1 || fail "the build with a matching [target.linux.build] row failed" build1.log
ninja1=$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)
[ -n "$ninja1" ] || fail "no build.ninja produced" build1.log

cxxflags_line=$(grep -E '^cxxflags' "$ninja1" | head -1)
echo "$cxxflags_line" | grep -qF -- '-DX717A' \
    || fail "the row's flag is not in build.ninja's global cxxflags (reaches neither scan nor TUs)" "$ninja1"
echo "$cxxflags_line" | grep -qF -- '-DBASE717' \
    || fail "the root's own unconditional flag is missing from cxxflags" "$ninja1"
# Order: the root's own [build] precedes the matching conditional row.
base_pos=$(echo "$cxxflags_line" | grep -bo -- '-DBASE717' | head -1 | cut -d: -f1)
cond_pos=$(echo "$cxxflags_line" | grep -bo -- '-DX717A' | head -1 | cut -d: -f1)
[ "$base_pos" -lt "$cond_pos" ] \
    || fail "the conditional row's flag does not follow the root's own [build] flag" "$ninja1"
echo "  ok: build.ninja's cxxflags carries -DBASE717 then -DX717A (reaches the scan and every TU)"

grep -qF -- '-DX717A' compile_commands.json \
    || fail "-DX717A is missing from compile_commands.json" compile_commands.json
echo "  ok: the flag reaches compile_commands.json"

stdA=$(std_module_json_for '-DX717A')
[ -n "$stdA" ] || fail "no std-module.json records a std_flag containing -DX717A" build1.log
grep -qF -- '-DBASE717' "$stdA" \
    || fail "the std module's cached prebuild command is missing the root's unconditional flag" "$stdA"
echo "  ok: the std BMI prebuild's recorded command carries both flags ($stdA)"

echo "== 813/2: a selector that does not match the resolved target contributes nothing =="

mkdir -p "$TMP/two/src"
cd "$TMP/two"
cat > mcpp.toml <<'TOML'
[package]
name     = "app717b"
version  = "0.1.0"
standard = "c++23"

[build]
dialect_cxxflags = ["-DBASE717"]

[targets.app717b]
kind = "bin"
main = "src/main.cpp"

[target.windows.build]
dialect_cxxflags = ["-DX717A"]
TOML
printf 'import std;\nint main() { std::println("ok"); return 0; }\n' > src/main.cpp

"$MCPP" build --toolchain gcc@16.1.0 > build2.log 2>&1 || fail "the build with a non-matching [target.windows.build] row failed" build2.log
ninja2=$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)
grep -qF -- '-DX717A' "$ninja2" compile_commands.json \
    && fail "-DX717A from a non-matching [target.windows.build] row reached a Linux build" "$ninja2"
grep -E '^cxxflags' "$ninja2" | grep -qF -- '-DBASE717' \
    || fail "the root's own unconditional flag is missing" "$ninja2"
echo "  ok: the non-matching row's flag reaches neither build.ninja nor compile_commands.json"

echo "== 813/3: switching between a matching and a non-matching row rebuilds the std BMI (A, then B, then A) =="

mkdir -p "$TMP/cyc/src"
cd "$TMP/cyc"
write_cyc() {   # $1 = the selector ("linux" or "windows")
    cat > mcpp.toml <<TOML
[package]
name     = "app717c"
version  = "0.1.0"
standard = "c++23"

[targets.app717c]
kind = "bin"
main = "src/main.cpp"

[target.$1.build]
dialect_cxxflags = ["-DCYC717"]
TOML
}
printf 'import std;\nint main() { std::println("ok"); return 0; }\n' > src/main.cpp

write_cyc linux
rm -rf target compile_commands.json
"$MCPP" build --toolchain gcc@16.1.0 > cyc-a1.log 2>&1 || fail "build A (matching) failed" cyc-a1.log
grep -qF -- '-DCYC717' "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)" \
    || fail "build A did not carry -DCYC717" cyc-a1.log
stdA1=$(std_module_json_for '-DCYC717')
[ -n "$stdA1" ] || fail "no std-module.json for build A" cyc-a1.log
echo "  ok: build A (linux, matching) carries -DCYC717 ($stdA1)"

# `target/` (and the project's own compile_commands.json) is removed between
# steps so that only ONE build.ninja / compile_commands.json ever exists at a
# time -- this scenario's subject is the SHARED std BMI cache under
# $MCPP_HOME/build-cache (content-addressed, independent of the project's own
# output directory), not the project's own directory reuse, which 813/4 below
# checks on its own terms.
write_cyc windows
rm -rf target compile_commands.json
"$MCPP" build --toolchain gcc@16.1.0 > cyc-b.log 2>&1 || fail "build B (non-matching) failed" cyc-b.log
grep -qF -- '-DCYC717' "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)" compile_commands.json \
    && fail "build B (windows, non-matching on a Linux host) still carries -DCYC717" cyc-b.log
echo "  ok: build B (windows, non-matching) carries no -DCYC717 -- a different std BMI"

write_cyc linux
rm -rf target compile_commands.json
"$MCPP" build --toolchain gcc@16.1.0 > cyc-a2.log 2>&1 || fail "build A2 (matching again) failed" cyc-a2.log
grep -qF -- '-DCYC717' "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)" \
    || fail "build A2 did not carry -DCYC717" cyc-a2.log
stdA2=$(std_module_json_for '-DCYC717')
[ -n "$stdA2" ] || fail "no std-module.json for build A2" cyc-a2.log
[ "$stdA2" = "$stdA1" ] \
    || fail "switching back to the matching row did not reuse build A's std BMI cache entry" "$stdA1" "$stdA2"
echo "  ok: switching back to linux reuses build A's std BMI cache entry exactly (A, then B, then A)"

echo "== 813/4: a dependency's own dialect_cxxflags reaches no command and does not move the graph's fingerprint =="

mkdir -p "$TMP/dep717/src" "$TMP/four/src"
cat > "$TMP/dep717/mcpp.toml" <<'TOML'
[package]
name     = "dep717"
version  = "0.1.0"
standard = "c++23"

[targets.dep717]
kind = "lib"

[build]
dialect_cxxflags = ["-DDEPFLAG_V1"]
TOML
printf 'export module dep717;\nexport int value() { return 42; }\n' > "$TMP/dep717/src/dep717.cppm"

cd "$TMP/four"
cat > mcpp.toml <<'TOML'
[package]
name     = "app717d"
version  = "0.1.0"
standard = "c++23"

[dependencies]
dep717 = { path = "../dep717" }

[targets.app717d]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'import dep717;\nint main() { return value() == 42 ? 0 : 1; }\n' > src/main.cpp

"$MCPP" build --toolchain gcc@16.1.0 > build4a.log 2>&1 || fail "the first build (dependency carries -DDEPFLAG_V1) failed" build4a.log
ninjas4a=$(find target -name build.ninja -not -path '*/.build-mcpp/*')
[ "$(echo "$ninjas4a" | wc -l)" -eq 1 ] || fail "more than one build.ninja after the first build" build4a.log
grep -qF -- '-DDEPFLAG_V1' "$ninjas4a" compile_commands.json \
    && fail "the dependency's own dialect_cxxflags reached a command" "$ninjas4a"
echo "  ok: the dependency's -DDEPFLAG_V1 reaches neither build.ninja nor compile_commands.json"

sed -i 's/-DDEPFLAG_V1/-DDEPFLAG_V2/' "$TMP/dep717/mcpp.toml"
"$MCPP" build --toolchain gcc@16.1.0 > build4b.log 2>&1 || fail "the second build (dependency's flag changed) failed" build4b.log
ninjas4b=$(find target -name build.ninja -not -path '*/.build-mcpp/*')
if [ "$(echo "$ninjas4b" | wc -l)" -ne 1 ]; then
    echo "build.ninja files after the second build:"; echo "$ninjas4b"
    fail "changing the dependency's own dialect_cxxflags left a second output directory behind (the fingerprint moved)" build4b.log
fi
if [ "$ninjas4b" != "$ninjas4a" ]; then
    echo "first build.ninja:  $ninjas4a"; echo "second build.ninja: $ninjas4b"
    fail "changing the dependency's own dialect_cxxflags moved the graph's output directory" build4b.log
fi
grep -qF -- '-DDEPFLAG_V2' "$ninjas4b" compile_commands.json \
    && fail "the dependency's changed dialect_cxxflags reached a command" "$ninjas4b"
echo "  ok: the same output directory is reused; the dependency's own key never entered the graph's fingerprint"

echo "PASS: 813"
