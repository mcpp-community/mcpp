#!/usr/bin/env bash
# requires: gcc
# 895 -- a member's conditional configuration rows count as they evaluate for
# the build (W4, design 2026-10-10 §3.5 item 5; #786).
#
#   R1  A member's own `[target.<sel>.build] dialect_cxxflags` reaches the
#       build when the member is planned under the workspace's virtual root.
#       2026.10.8.1 dropped it: the plan's root carried none of the member's
#       rows (`DIAL=0` with `-p app`, `DIAL=2` for the same package alone).
#   R2  Two members that differ only in a row for another target are one
#       configuration: `--workspace` plans one graph.
#   R3  Two members whose rows evaluate differently for this target are two.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

HOST='cfg(any(os = "linux", os = "macos", os = "windows"))'
member() {  # $1 = name, $2 = extra manifest text
    mkdir -p "$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n%s\n' \
        "$1" "$1" "$2" > "$1/mcpp.toml"
    printf '#include <cstdio>\n#ifndef DIAL\n#define DIAL 0\n#endif\nint main() { std::printf("DIAL=%%d\\n", DIAL); }\n' > "$1/src/main.cpp"
}
run_of() { local b; b=$(find target -path "*/bin/$1/$1" -type f -o -path "*/bin/$1" -type f -o -path "*/bin/$1.exe" -type f | head -1); "$b" | tr -d '\r'; }

# ── R1 ────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/r1" && cd "$TMP/r1"
printf '[workspace]\nmembers = ["app"]\n' > mcpp.toml
member app "[target.'$HOST'.build]
dialect_cxxflags = [\"-DDIAL=2\"]"
"$MCPP" build -p app > b.log 2>&1 || fail "build" b.log
got=$(run_of app)
[ "$got" = "DIAL=2" ] || fail "the member's own conditional dialect_cxxflags did not reach the build: $got" b.log
echo "ok R1"

# ── R2, R3 ────────────────────────────────────────────────────────────────
mkdir -p "$TMP/r2" && cd "$TMP/r2"
printf '[workspace]\nmembers = ["a", "b"]\n' > mcpp.toml
member a "[target.'cfg(os = \"none\")'.build]
dialect_cxxflags = [\"-DNEVER=1\"]"
member b ""
"$MCPP" build --workspace > ws.log 2>&1 || fail "--workspace" ws.log
n=$(find target -name build.ninja | wc -l | tr -d ' ')
[ "$n" = 1 ] || fail "a row for another target split the members into $n configurations" ws.log

mkdir -p "$TMP/r3" && cd "$TMP/r3"
printf '[workspace]\nmembers = ["a", "b"]\n' > mcpp.toml
member a "[target.'$HOST'.build]
dialect_cxxflags = [\"-DDIAL=3\"]"
member b ""
"$MCPP" build --workspace > ws.log 2>&1 || fail "--workspace" ws.log
n=$(find target -name build.ninja | wc -l | tr -d ' ')
[ "$n" = 2 ] || fail "members whose rows evaluate differently share $n configuration(s)" ws.log
[ "$(run_of a)" = "DIAL=3" ] || fail "member a lost its dialect: $(run_of a)" ws.log
[ "$(run_of b)" = "DIAL=0" ] || fail "member b received a's dialect: $(run_of b)" ws.log
echo "ok R2 R3"

echo "PASS: 895_conditional_configuration_rows_count_as_they_evaluate"
