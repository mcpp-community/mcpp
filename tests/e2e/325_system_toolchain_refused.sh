#!/usr/bin/env bash
# requires: gcc
# 325_system_toolchain_refused.sh — mcpp builds only with toolchains it
# manages (#527 Bug 1).
#
# THE POLICY, AND WHY IT IS NOT UNIFORM ACROSS AXES.
#
#   LIBRARIES are the program's business. A project may link a host library or
#   its own `.so`; mcpp says what that costs and does not refuse, because the
#   developer owns the artifact and guarantees it.
#
#   THE TOOLCHAIN is mcpp's own contract. Everything mcpp promises — that
#   `import std` is available, that the runtime closure is computable, that two
#   machines and CI produce the same build — is a statement about a compiler
#   mcpp resolved and can identify. A compiler picked off PATH makes every one
#   of those unverifiable, so `[toolchain] … = "system"` is refused.
#
# `msvc@system` is the single exception and is a different spelling: it names a
# FAMILY whose installation mcpp locates, on the one platform where the
# compiler cannot be redistributed.
#
# What this replaces: the same configuration used to die as
# `posix_spawnp('') failed (error 2)` as soon as the project had a build.mcpp,
# because the resolved compiler path sat in `tc->binaryPath` and was never
# handed to the build.mcpp closure. A refusal that arrives as a crash three
# layers down is not a policy, it is a bug wearing one.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src
printf '#include <cstdio>\nint main(){ std::printf("hi\\n"); return 0; }\n' > src/main.cpp

manifest() {  # $1 = extra body
    printf '[package]\nname = "systc"\nversion = "0.1.0"\nstandard = "c++23"\n%s' "$1" > mcpp.toml
}

expect_refusal() {  # $1 = log file, $2 = what was being built
    grep -q 'toolchains it manages' "$1" || {
        echo "FAIL: $2 was not refused on the toolchain policy's own terms"
        cat "$1"; exit 1; }
    # A refusal has to be actionable, or it is only a complaint. Three things
    # make it so: what to write instead, where to see the choices, and the one
    # exception — a Windows user reading a blanket "system is not supported"
    # would otherwise conclude `msvc@system` had been removed too.
    grep -q 'gcc@16.1.0' "$1" || {
        echo "FAIL: the refusal does not show what to write instead"; cat "$1"; exit 1; }
    grep -q 'mcpp toolchain' "$1" || {
        echo "FAIL: the refusal does not name the command that lists the choices"
        cat "$1"; exit 1; }
    grep -q 'msvc@system' "$1" || {
        echo "FAIL: the refusal does not name the one supported exception"
        cat "$1"; exit 1; }
    # And it must not read as a ban on host libraries, which are a different
    # axis with the opposite answer.
    grep -qi 'librar' "$1" || {
        echo "FAIL: the refusal does not distinguish the library axis"
        cat "$1"; exit 1; }
}

# ── refused without a build.mcpp ────────────────────────────────────────────
manifest '
[toolchain]
linux   = "system"
macos   = "system"
windows = "system"
'
rm -rf target
if "$MCPP" build > plain.log 2>&1; then
    echo "FAIL: [toolchain] system built; mcpp builds only with managed toolchains"
    cat plain.log; exit 1
fi
expect_refusal plain.log "a plain project"

# ── refused WITH a build.mcpp, and not as a spawn failure ───────────────────
#
# THE ORIGINAL DEFECT. The refusal has to reach the user before anything tries
# to compile the build program, or they get exit 127 and an empty program name.
printf '#include <cstdio>\nint main(){ return 0; }\n' > build.mcpp
rm -rf target
if "$MCPP" build > bp.log 2>&1; then
    echo "FAIL: [toolchain] system with a build.mcpp built"
    cat bp.log; exit 1
fi
expect_refusal bp.log "a project with a build.mcpp"
grep -q "posix_spawnp" bp.log && {
    echo "FAIL: still spawning an empty compiler path instead of refusing"
    cat bp.log; exit 1; }
grep -q "build.mcpp compiling" bp.log && {
    echo "FAIL: the refusal arrived after the build program had started"
    cat bp.log; exit 1; }

# ── the same via the environment side channel ───────────────────────────────
rm -f build.mcpp
manifest ''
rm -rf target
if MCPP_TOOLCHAIN=system "$MCPP" build > env.log 2>&1; then
    echo "FAIL: MCPP_TOOLCHAIN=system built; the policy must not have two answers"
    cat env.log; exit 1
fi
expect_refusal env.log "MCPP_TOOLCHAIN=system"

# ── THE DENOMINATOR: a managed toolchain still builds ───────────────────────
#
# Without this, "everything is refused" satisfies every assertion above.
manifest ''
rm -rf target
"$MCPP" build > managed.log 2>&1 || {
    echo "FAIL: a project with no [toolchain] at all must still build"
    cat managed.log; exit 1
}
grep -q 'toolchains it manages' managed.log && {
    echo "FAIL: the refusal fired for a project that did not ask for system"
    cat managed.log; exit 1; }

echo "PASS: 325_system_toolchain_refused"
