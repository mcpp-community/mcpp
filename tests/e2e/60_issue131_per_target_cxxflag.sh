#!/usr/bin/env bash
# requires: gcc
# Issue #131 scenario 1, verbatim shape: a single bin target carries a per-target
# *codegen* cxxflag that must affect ONLY its own entry `main`, not shared code:
#
#   [targets.test_contracts]
#   kind = "bin"
#   main = "tests/test_contracts.cpp"
#   cxxflags = ["-fcontract-evaluation-semantic=observe"]
#
# We use `-fno-exceptions` as a portable stand-in for the contracts semantic
# flag (which needs a contracts-enabled toolchain not guaranteed across the
# Linux/macOS/Windows CI matrix). The mechanism under test is identical: a real
# per-target codegen flag scoped to that target's exclusive entry. Its effect is
# preprocessor-observable via `__EXCEPTIONS` (defined iff exceptions are on), so
# both "reached the entry" and "did NOT leak to shared" are checked at compile
# time, plus the flag is asserted on the target's main edge in build.ninja.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p proj/src proj/tests
cd proj

# Shared source — globbed, compiled once with DEFAULT flags (exceptions on).
# If the per-target flag leaked here, __EXCEPTIONS would be undefined → error.
cat > src/shared.cpp <<'EOF'
#ifndef __EXCEPTIONS
#error "per-target cxxflag leaked into a shared compile unit"
#endif
extern "C" int shared_val(void) { return 42; }
EOF

# The target's own entry — compiled WITH the per-target flag (-fno-exceptions),
# so __EXCEPTIONS must be undefined here.
cat > tests/test_contracts.cpp <<'EOF'
#ifdef __EXCEPTIONS
#error "per-target cxxflag did not reach this target's own entry"
#endif
extern "C" int shared_val(void);
int main() { return shared_val() == 42 ? 0 : 1; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "proj"
version = "0.1.0"

[targets.test_contracts]
kind     = "bin"
main     = "tests/test_contracts.cpp"
cxxflags = ["-fno-exceptions"]
EOF

# Compiles only if the flag reached the entry (entry check) AND did not leak to
# the shared unit (shared check) — both are #error guards above.
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }

ninja_file="$(find target -name build.ninja | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja"; exit 1; }
grep -q -- "-fno-exceptions" "$ninja_file" || { echo "per-target cxxflag missing from build.ninja"; exit 1; }

"$MCPP" run test_contracts > /dev/null 2>&1 || { echo "run failed"; exit 1; }

echo "OK"
