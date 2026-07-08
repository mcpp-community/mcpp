#!/usr/bin/env bash
# requires: macos
# 94_macos_test_stdlib.sh — A1 regression: test binaries must link the
# TOOLCHAIN's libc++, not the system one. The old system -lc++ exception
# split headers (toolchain libc++) from the dylib (Apple's older libc++)
# and broke on libc++ 22's out-of-line __hash_memory — exercised here via
# std::unordered_map<std::string,...> string hashing.
# Design: .agents/docs/2026-07-08-root-cause-remediation-design.md A1.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new hashapp > /dev/null
cd hashapp

mkdir -p tests
cat > tests/hash_test.cpp <<'EOF'
// Forces the string-hash path (__hash_memory-class symbols on libc++ >= 22).
import std;

int main() {
    std::unordered_map<std::string, int> m;
    for (int i = 0; i < 100; ++i) m["key" + std::to_string(i)] = i;
    return (m.size() == 100 && m.at("key42") == 42) ? 0 : 1;
}
EOF

"$MCPP" test > test.log 2>&1 || { cat test.log; echo "FAIL: mcpp test failed"; exit 1; }
grep -Eq "[0-9]+ passed; 0 failed" test.log   # template ships its own test_smoke too

# The link assertion only applies to clang/llvm toolchains (gcc on macOS is
# not a supported config; if resolution picked something else, the behavior
# test above already covered the regression).
TEST_BIN=$(find target -path "*/bin/hash_test" | head -1)
test -x "$TEST_BIN"
if grep -q "Resolved llvm@" test.log || otool -L "$TEST_BIN" | grep -q "libc++"; then
    echo "otool -L:"
    otool -L "$TEST_BIN"
    if otool -L "$TEST_BIN" | grep -q "/usr/lib/libc++"; then
        echo "FAIL: test binary links the SYSTEM libc++ (header/dylib split)"
        exit 1
    fi
    # Either the toolchain's dylib (rpath link) or no libc++ dylib at all
    # (static fallback) is acceptable — never Apple's.
fi

echo "PASS 94_macos_test_stdlib"
