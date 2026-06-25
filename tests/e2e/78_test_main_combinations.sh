#!/usr/bin/env bash
# requires:
# 78_test_main_combinations.sh — `mcpp test` must handle every combination of
# {test defines its own main, test relies on the framework's main} ×
# {test uses gtest, test doesn't} when gtest is a dev-dependency.
#
# Regression for `ld.lld: duplicate symbol: main`: gtest's gtest_main.cc carries
# its own main(), and mcpp used to inline ALL of a dev-dep's objects into every
# test binary — so a test that defined its own main() collided with gtest_main.
# The fix links kind="lib" dependencies as a static archive (lib<pkg>.a) placed
# AFTER the test's objects, so the linker pulls gtest_main.o ONLY when main is
# still undefined. Driven by the dep's declared kind — no gtest special-casing.
#
# No `requires:` capability → runs on all three CI platforms (mirrors 15/16/17).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new combo > /dev/null
cd combo

cat >> mcpp.toml <<'EOF'

[dev-dependencies]
gtest = "1.15.2"
EOF

rm -f tests/test_smoke.cpp

# (1) own main + uses gtest → gtest_main.o must NOT be pulled (no dup main)
cat > tests/t_own_main_gtest.cpp <<'EOF'
#include <gtest/gtest.h>
import std;
TEST(A, ok) { EXPECT_EQ(1 + 1, 2); }
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
EOF

# (2) no main + uses gtest macros → gtest_main.o provides main
cat > tests/t_framework_main.cpp <<'EOF'
#include <gtest/gtest.h>
TEST(B, ok) { EXPECT_TRUE(true); }
EOF

# (3) own main + does NOT use gtest (but gtest is still a dev-dep) → archive
#     contributes nothing; previously this collided with gtest_main's main.
cat > tests/t_own_main_no_gtest.cpp <<'EOF'
import std;
int main() { std::println("ok"); return 0; }
EOF

out=$("$MCPP" test 2>&1) || { echo "FAIL: mcpp test exited non-zero"; echo "$out"; exit 1; }

for t in t_own_main_gtest t_framework_main t_own_main_no_gtest; do
    echo "$out" | grep -q "$t ... ok" || { echo "FAIL: $t did not pass"; echo "$out"; exit 1; }
done
echo "$out" | grep -q '3 passed; 0 failed' || { echo "FAIL: summary mismatch"; echo "$out"; exit 1; }

# A test that defines its own main must NOT also link gtest_main.o (that would be
# `duplicate symbol: main`); a test relying on the framework's main MUST link it.
nj=$(find target -name build.ninja | head -1)
own_link="$(grep 'bin/t_own_main_gtest :' "$nj" || true)"
fw_link="$(grep 'bin/t_framework_main :' "$nj" || true)"
echo "$own_link" | grep -q 'gtest_main' && { echo "FAIL: own-main test links gtest_main.o (dup main)"; exit 1; }
echo "$fw_link" | grep -q 'gtest_main' || { echo "FAIL: framework-main test missing gtest_main.o"; exit 1; }

echo "OK"
