#!/usr/bin/env bash
# requires: elf
# 904 -- features are activated per instance (mcpp#797, R3).
#
# A feature one package requests of a library in its [build-dependencies] is
# a request of the library's build-program instance. It used to be unioned
# with every other request of the package, so it reached the target instance:
# the target compile of the library carried the feature's define.
#
#   A  the target instance of `hl` does not have feature `x`, which only
#      `mid`'s [build-dependencies] requests;
#   B  the build-program instance has it: mid's build program sees `x`.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/hl/src" "$TMP/mid/src" "$TMP/app/src"
cat > "$TMP/hl/src/hl.cppm" <<'EOF'
export module hl;
export namespace hl {
inline int x_on() {
#ifdef HL_X
    return 1;
#else
    return 0;
#endif
}
}
EOF
printf '[package]\nname = "hl"\nversion = "0.1.0"\n[features]\nx = { defines = ["HL_X"] }\n[targets.hl]\nkind = "lib"\n' > "$TMP/hl/mcpp.toml"
printf '[package]\nname = "mid"\nversion = "0.1.0"\n[build-dependencies]\nhl = { path = "../hl", features = ["x"] }\n[targets.mid]\nkind = "lib"\n' > "$TMP/mid/mcpp.toml"
printf 'export module mid;\nexport int mid_x() { return MID_X; }\n' > "$TMP/mid/src/mid.cppm"
printf 'import std;\nimport mcpp;\nimport hl;\nint main() { mcpp::cxxflag(std::format("-DMID_X={}", hl::x_on()).c_str()); }\n' > "$TMP/mid/build.mcpp"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\nhl = { path = "../hl" }\nmid = { path = "../mid" }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app/mcpp.toml"
printf '#include <cstdio>\nimport hl;\nimport mid;\nint main() { std::printf("target=%%d program=%%d\\n", hl::x_on(), mid_x()); }\n' > "$TMP/app/src/main.cpp"

cd "$TMP/app"
"$MCPP" build > b.log 2>&1 || fail "the build failed" b.log
out="$("$MCPP" run 2>/dev/null | grep '^target=' | tail -1)"
[ "$out" = "target=0 program=1" ] || fail "A/B: $out (want target=0 program=1)" b.log
echo "PASS: 904_a_build_dependency_feature_stays_out_of_the_target"
