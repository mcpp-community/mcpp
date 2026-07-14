#!/usr/bin/env bash
# 101_cppfly_llvm_soft.sh — standard = "c++fly" soft path on Clang (design
# §5.4): upstream Clang has no reflection/contracts, so the build must still
# succeed at the family's latest level (-std=c++2c), with the skipped gates
# reported in the summary — mcpp diagnostics instead of raw compiler errors.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

source "$(dirname "$0")/_llvm_env.sh"
if [[ ! -d "$LLVM_ROOT" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION:-none} is not installed"
    exit 0
fi

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<EOF
[package]
name     = "flysoft"
version  = "0.1.0"
standard = "c++fly"

[toolchain]
default = "llvm@${LLVM_VERSION}"
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("fly soft ok"); }
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: c++fly soft-path build failed on llvm"
    exit 1
}

grep -q 'c++fly on clang' "$TMP/build.log" || {
    cat "$TMP/build.log"
    echo "FAIL: build log missing c++fly summary line"
    exit 1
}
grep -q 'skipped: .*reflection' "$TMP/build.log" || {
    cat "$TMP/build.log"
    echo "FAIL: summary does not report reflection as skipped on clang"
    exit 1
}

build_ninja="$(find target -name build.ninja | head -1)"
grep -qE '^cxxflags  = -std=c\+\+2c' "$build_ninja" || {
    grep '^cxxflags' "$build_ninja" || true
    echo "FAIL: clang c++fly did not resolve to -std=c++2c"
    exit 1
}

# stdlib gate: libc++ (linux/macos llvm payloads) → enabled + flag present;
# clang on MSVC STL (windows) → reported skipped + flag absent. Assert the
# summary and the emitted flags AGREE rather than hardcoding one stdlib.
if grep -q 'experimental-library (-fexperimental-library)' "$TMP/build.log"; then
    grep -q -- '-fexperimental-library' "$build_ninja" || {
        echo "FAIL: summary claims experimental-library but build.ninja lacks the flag"
        exit 1
    }
else
    grep -q 'skipped: .*experimental-library' "$TMP/build.log" || {
        cat "$TMP/build.log"
        echo "FAIL: experimental-library neither enabled nor reported skipped"
        exit 1
    }
    if grep -q -- '-fexperimental-library' "$build_ninja"; then
        echo "FAIL: -fexperimental-library emitted for a non-libc++ stdlib"
        exit 1
    fi
fi

binary=$(find target -type f \( -path '*/bin/flysoft' -o -path '*/bin/flysoft.exe' \) | head -1)
out=$("$binary")
[[ "$out" == *"fly soft ok"* ]] || {
    echo "FAIL: runtime output: $out"
    exit 1
}

echo "PASS: c++fly soft path on clang — latest level + skipped summary"
