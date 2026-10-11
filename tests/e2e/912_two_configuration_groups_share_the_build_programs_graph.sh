#!/usr/bin/env bash
# requires: unix-shell
# 912 -- two configuration groups of one command whose build programs have one
# host configuration share the build programs' graph (mcpp#797).
#
# The groups of a workspace command build at the same time. Their build
# programs are compiled for the machine doing the build, with one toolchain and
# one standard, so they are planned in one directory: one group compiles, the
# other waits for its lock and finds the libraries compiled.
#
#   A  members `a` and `b` differ in [profile.dev], so they are two groups; both
#      build, and each program sees the library both import;
#   B  the library is compiled once, in one build programs' graph.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/shared/src" "$TMP/ws"
printf '[package]\nname = "shared"\nversion = "0.1.0"\n[targets.shared]\nkind = "lib"\n' > "$TMP/shared/mcpp.toml"
printf 'export module shared;\nexport int shared_value() { return 42; }\n' > "$TMP/shared/src/shared.cppm"
cd "$TMP/ws"
printf '[workspace]\nmembers = ["a", "b"]\n' > mcpp.toml
for m in a b; do
    mkdir -p $m/src
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n[build-dependencies]\nshared = { path = "../../shared" }\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' $m $m > $m/mcpp.toml
    printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > $m/src/main.cpp
    printf 'import std;\nimport mcpp;\nimport shared;\nint main() { mcpp::cxxflag(std::format("-DV={}", shared_value()).c_str()); }\n' > $m/build.mcpp
done
printf '[profile.dev]\nopt = 1\n' >> a/mcpp.toml

# A
"$MCPP" build --workspace > a.log 2>&1 || fail "A: the workspace did not build" a.log
[ "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | wc -l | tr -d ' ')" = 2 ] \
    || fail "A: the members were not two configuration groups" a.log
for m in a b; do
    [ "$("$MCPP" run -p $m 2>/dev/null | grep '^V=' | tail -1)" = "V=42" ] || fail "A: $m's value" a.log
done

# B
[ "$(find target/.build-mcpp/host -mindepth 1 -maxdepth 1 -type d -not -name digests | wc -l | tr -d ' ')" = 1 ] \
    || fail "B: the groups' build programs were planned in more than one directory" a.log
compiles=$(find target/.build-mcpp/host -name .ninja_log -exec awk -F'\t' '$4 ~ /shared\.m\.o$/' {} + | wc -l | tr -d ' ')
[ "$compiles" = 1 ] || fail "B: the library was compiled $compiles times" a.log
echo "PASS: 912_two_configuration_groups_share_the_build_programs_graph"
