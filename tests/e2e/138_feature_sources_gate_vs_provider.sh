#!/usr/bin/env bash
# requires: unix-shell
# A feature that GATES a source and a feature that PROVIDES one, under both
# `mcpp build` and `mcpp test`.
#
# ⚠️ THIS DEFECT SURVIVED FOR MONTHS BECAUSE NO TEST COVERED BOTH FAMILIES, AND
# THREE ATTEMPTED FIXES EACH BROKE THE ONE THEY DID NOT COVER.
#
# Two shapes of package reach the same code in `prepare_build`, and under
# `mcpp test` they want opposite things:
#
#   the GATE family      gtest lists `*/googletest/src/gtest_main.cc` in base
#                        `sources` AND under `features.main`. The package
#                        provides the file unconditionally; the feature is a
#                        switch over it. The dev-dependency track's per-test
#                        main detection has to SEE it to prune it per test, so
#                        an inactive gate must not make it disappear.
#
#   the PROVIDER family  riscv-virt-rt names `src/kal/**` under
#                        `features.openkal` and nowhere else. Those files are
#                        not part of the package without the feature — the
#                        headers they include arrive through that feature's
#                        `[feature-deps]` — so compiling them fails on a header
#                        that was never meant to be there.
#
# The engine gated the whole exclusion on "is this a test build", which is
# right for the first family and wrong for the second. Fixing it by removing
# the gate broke gtest; fixing it by removing only the glob string was a no-op,
# because a provider's glob is not in base `sources` at all and its files are
# matched by the inferred `src/**`.
#
# Both families are asserted here so that the next change to this code cannot
# satisfy one at the other's expense.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── The PROVIDER family ─────────────────────────────────────────────────────
#
# `src/gated/**` is named only by the feature, and the file includes a header
# that does not exist. With the feature inactive both commands must ignore it;
# a command that compiles it fails on the missing header, which is exactly the
# symptom the real package produced.
mkdir -p provider/src/gated provider/tests
cat > provider/mcpp.toml <<'EOF'
[package]
name    = "provider"
version = "0.1.0"

# ⚠️ NO `[build] sources`, WHICH IS THE POINT. The base set is the inferred
# `src/**`, which matches `src/gated/only_with_feature.cpp` — so removing the
# feature's glob STRING from a list it was never in gates nothing.
[features]
default = []
gated   = { sources = ["src/gated/**"] }
EOF
cat > provider/src/lib.cpp <<'EOF'
int provider_value() { return 7; }
EOF
cat > provider/src/gated/only_with_feature.cpp <<'EOF'
// The header arrives with the feature's dependencies and does not exist
// without it. Compiling this file without the feature is the defect.
#include <a_header_the_feature_would_have_brought.h>
int gated_value() { return 1; }
EOF
cat > provider/tests/basic.cpp <<'EOF'
extern int provider_value();
int main() { return provider_value() == 7 ? 0 : 1; }
EOF

cd provider
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "provider: build compiled a feature-only source"; exit 1; }
"$MCPP" test  > test.log  2>&1 || { cat test.log;  echo "provider: TEST compiled a feature-only source — the defect this file exists for"; exit 1; }

if find target -name '*only_with_feature*' | grep -q .; then
    find target -name '*only_with_feature*'
    echo "provider: an object was produced for a source the feature did not activate"
    exit 1
fi

# With the feature ON it must be reached — otherwise the check above would pass
# for a build that simply ignores feature sources entirely.
if "$MCPP" build --features gated > gated.log 2>&1; then
    cat gated.log
    echo "provider: the feature did not bring its own source in"
    exit 1
fi
grep -q "a_header_the_feature_would_have_brought.h" gated.log || {
    cat gated.log
    echo "provider: activating the feature failed for some other reason"; exit 1; }
cd ..

# ── The GATE family ─────────────────────────────────────────────────────────
#
# The same glob in base `sources` and under a feature. `mcpp build` excludes it;
# `mcpp test` keeps it visible, which is what the dev-dependency track's
# per-test main detection depends on.
mkdir -p gate/src gate/tests
cat > gate/mcpp.toml <<'EOF'
[package]
name    = "gate"
version = "0.1.0"

# ⚠️ `src/gated_main.cpp` IS IN BOTH LISTS, EXACTLY AS gtest'S DESCRIPTOR HAS IT.
# The package provides the file; the feature is a switch over it.
[build]
sources = ["src/lib.cpp", "src/gated_main.cpp"]

[features]
default = []
main    = { sources = ["src/gated_main.cpp"] }
EOF
cat > gate/src/lib.cpp <<'EOF'
int gate_value() { return 11; }
EOF
cat > gate/src/gated_main.cpp <<'EOF'
int gated_main_marker() { return 42; }
EOF
cat > gate/tests/basic.cpp <<'EOF'
extern int gate_value();
int main() { return gate_value() == 11 ? 0 : 1; }
EOF

cd gate
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "gate: build failed"; exit 1; }
if find target -name '*gated_main*' | grep -q .; then
    find target -name '*gated_main*'
    echo "gate: build compiled a gated source with the feature inactive"
    exit 1
fi

rm -rf target
"$MCPP" test > test.log 2>&1 || { cat test.log; echo "gate: test failed"; exit 1; }
# ⚠️ THE ASSERTION IS THAT IT IS STILL THERE. A fix that excluded every
# inactive feature glob under `mcpp test` would remove it — and would then
# break gtest, whose per-test main detection needs to see the file in order to
# prune it. Measured once as `ld returned 1 exit status` across mcpp's own
# unit tests.
find target -name '*gated_main*' | grep -q . || {
    echo "gate: a source the package provides unconditionally vanished under \`mcpp test\`"
    echo "      (this is the shape that broke gtest)"
    exit 1; }
cd ..

echo "PASS: a feature that gates a source and a feature that provides one behave correctly under both commands"
