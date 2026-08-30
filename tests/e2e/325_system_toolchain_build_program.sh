#!/usr/bin/env bash
# requires: gcc
# 325_system_toolchain_build_program.sh — `[toolchain] system` with a
# build.mcpp (#527 Bug 1).
#
# `explicit_compiler` is assigned by every branch that resolves a toolchain from
# the index and NOT by the `system` branch, which has nothing to assign until
# `detect` finds the PATH compiler and stores its absolute path in
# `tc->binaryPath`. The main build reads the compiler from `tc` and worked; the
# build.mcpp closure returned the local variable and handed "" to
# `posix_spawnp`:
#
#     error: build.mcpp failed to compile (exit 127):
#     posix_spawnp('') failed (error 2): No such file or directory
#
# WARNED, NOT REFUSED. mcpp itself depends on no host — its toolchains and
# everything the ecosystem publishes are resolved from the xim index — but a
# USER'S OWN project may decide otherwise, and that decision is theirs to
# guarantee. The boundary is whether the result builds and runs, and this one
# does. So the escape hatch is made consistent and the cost is stated once.
set -e

command -v g++ >/dev/null 2>&1 || { echo "SKIP: no host g++ on PATH"; exit 0; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src

printf '[package]\nname = "sysbp"\nversion = "0.1.0"\nstandard = "c++23"\n\n[toolchain]\nlinux = "system"\nmacos = "system"\nwindows = "system"\n' > mcpp.toml
printf '#include <cstdio>\nint main(){ std::printf("hi\\n"); return 0; }\n' > src/main.cpp

# ── without a build.mcpp: unchanged, and it must stay that way ──────────────
#
# THE DENOMINATOR. This configuration builds today; a fix written as a blanket
# refusal of `[toolchain] system` would pass every other assertion here while
# breaking working setups.
"$MCPP" build > plain.log 2>&1 || {
    echo "FAIL: [toolchain] system without a build.mcpp must keep building"
    cat plain.log; exit 1
}
grep -q "toolchain" plain.log && grep -qi "warning" plain.log || {
    echo "FAIL: the host-dependence warning did not fire"
    cat plain.log; exit 1
}
# The warning has to name the way back, or it is only a complaint.
grep -q "xim index" plain.log || {
    echo "FAIL: the warning does not name the supported route"
    cat plain.log; exit 1
}

# ── with a build.mcpp: this is the crash ────────────────────────────────────
printf '#include <cstdio>\nint main(){ return 0; }\n' > build.mcpp
rm -rf target
"$MCPP" build > bp.log 2>&1 || {
    echo "FAIL: [toolchain] system with a build.mcpp must build"
    cat bp.log; exit 1
}
grep -q "posix_spawnp" bp.log && {
    echo "FAIL: the empty compiler path is still being spawned"
    cat bp.log; exit 1
}

# ── one warning per build, and none for a compliant project ────────────────
#
# A policy warning that fires on a project doing the right thing teaches people
# to ignore the channel, which costs more than the warning buys.
n=$(grep -c "selects a compiler from PATH" bp.log || true)
[ "$n" -le 1 ] || { echo "FAIL: the warning fired $n times"; cat bp.log; exit 1; }

printf '[package]\nname = "sysbp"\nversion = "0.1.0"\nstandard = "c++23"\n' > mcpp.toml
rm -rf target
"$MCPP" build > clean.log 2>&1 || { cat clean.log; exit 1; }
grep -q "selects a compiler from PATH" clean.log && {
    echo "FAIL: the host-dependence warning fired for a project with a"
    echo "      declared toolchain"
    cat clean.log; exit 1
}

echo "PASS: 325_system_toolchain_build_program"
