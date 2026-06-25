#!/usr/bin/env bash
# requires:
# 79_gtest_regular_dep_feature_main.sh — gtest as a REGULAR dependency
# (`[dependencies]`, via `mcpp add gtest`) must NOT inject gtest_main into a
# `mcpp build` app that has its own main. Regression for issue #168
# (`gtest_main.o : error LNK2005: main already defined in main.o`).
#
# gtest_main.cc is gated behind the `main` feature in compat.gtest.lua: off by
# default (framework only) → no main collision; `features = ["main"]` opts in.
#
# No `requires:` capability → runs on all three CI platforms (the original bug
# was Windows/MSVC LNK2005). Depends on the published mcpp-index carrying the
# gtest `main` feature.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app

# (1) #168: gtest in [dependencies] + app's own main → build must succeed, and
#     gtest_main must NOT be linked.
"$MCPP" add gtest@1.15.2 > /dev/null
grep -q '^\[dependencies\]' mcpp.toml || { echo "FAIL: add did not write [dependencies]"; cat mcpp.toml; exit 1; }
"$MCPP" build > /dev/null || { echo "FAIL: #168 — build with regular-dep gtest failed"; exit 1; }
nj=$(find target -name build.ninja | xargs ls -t 2>/dev/null | head -1)
if grep -q 'gtest_main' "$nj"; then
    echo "FAIL: gtest_main linked into app by default (would collide with main)"; exit 1
fi
grep -q 'gtest-all' "$nj" || { echo "FAIL: gtest framework (gtest-all) not linked"; exit 1; }

# (2) opt-in: features = ["main"] + a TEST-only file (no own main) → gtest_main
#     IS linked and provides the entry.
cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
gtest = { version = "1.15.2", features = ["main"] }
EOF
cat > src/main.cpp <<'EOF'
#include <gtest/gtest.h>
TEST(App, ok) { EXPECT_EQ(1 + 1, 2); }
EOF
"$MCPP" build > /dev/null || { echo "FAIL: features=[main] build failed"; exit 1; }
nj=$(find target -name build.ninja | xargs ls -t 2>/dev/null | head -1)
grep -q 'gtest_main' "$nj" || { echo "FAIL: features=[main] did not link gtest_main"; exit 1; }

# (3) `mcpp add --dev` routes to [dev-dependencies].
cd "$TMP"
"$MCPP" new libapp > /dev/null
cd libapp
"$MCPP" add --dev gtest@1.15.2 > /dev/null
grep -q '^\[dev-dependencies\]' mcpp.toml || { echo "FAIL: add --dev did not write [dev-dependencies]"; cat mcpp.toml; exit 1; }

echo "OK"
