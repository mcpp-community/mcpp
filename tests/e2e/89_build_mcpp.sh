#!/usr/bin/env bash
# 89_build_mcpp.sh — L3 `build.mcpp`: a project-local native imperative build
# program (Zig build.zig / Cargo build.rs model, in C++). mcpp compiles it with
# the host toolchain and runs it before the main build; its stdout `mcpp:`
# directives augment the build (here: a cxxflag define + a generated source). A
# declared-input cache re-runs it only when its source / inputs / env change.
# See .agents/docs/2026-06-30-l3-build-mcpp-implementation-design.md.
#
# requires: gcc
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cd app

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF

# main.cpp hard-asserts the cxxflag define reached this TU, and links a function
# that only exists in the generated source the build program writes.
cat > src/main.cpp <<'EOF'
#ifndef FROM_BUILD_MCPP
#error "build.mcpp cxxflag did not reach the translation unit"
#endif
int generated_value();
int main() { return generated_value() == 42 ? 0 : 1; }
EOF

# The build program: writes a generated source, emits a define, declares an env input.
cat > build.mcpp <<'EOF'
#include <cstdio>
#include <fstream>
int main() {
    std::ofstream("src/generated.cpp") << "int generated_value() { return 42; }\n";
    std::puts("mcpp:cxxflag=-DFROM_BUILD_MCPP=1");
    std::puts("mcpp:generated=src/generated.cpp");
    std::puts("mcpp:rerun-if-env-changed=MCPP_TEST_TOGGLE");
    return 0;
}
EOF

# ── Build 1: first run — compiles+runs build.mcpp, applies directives ──────
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build 1 errored"; exit 1; }
grep -q "build.mcpp" b1.log || { cat b1.log; echo "FAIL: build.mcpp not invoked"; exit 1; }
[ -f src/generated.cpp ] || { echo "FAIL: generated source not written"; exit 1; }
[ -f target/.build-mcpp/build.mcpp.cache ] || { echo "FAIL: cache not written under target/"; exit 1; }

# The binary returns 0 only if both the define AND the generated source took effect.
"$MCPP" run > r1.log 2>&1 || { cat r1.log; echo "FAIL: run returned non-zero (define/generated source missing)"; exit 1; }

# ── Build 2: touch a source so prepare runs again, but build.mcpp inputs are
#    unchanged — the declared-input cache short-circuits the re-run. (A no-change
#    rebuild is skipped wholesale by the top-level up-to-date check, so we touch a
#    source to actually exercise the prepare path.)
touch src/main.cpp
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: build 2 errored"; exit 1; }
grep -qi "build.mcpp.*cached" b2.log || { cat b2.log; echo "FAIL: build.mcpp did not short-circuit (expected cached)"; exit 1; }

# ── Build 3: a declared env input changed — forces a re-run ────────────────
touch src/main.cpp
MCPP_TEST_TOGGLE=1 "$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: build 3 errored"; exit 1; }
grep -qi "build.mcpp.*\(running\|compiling\)" b3.log || { cat b3.log; echo "FAIL: changed env did not force build.mcpp re-run"; exit 1; }

echo "OK"
