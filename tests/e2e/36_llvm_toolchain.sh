#!/usr/bin/env bash
# 36_llvm_toolchain.sh — build a non-module C/C++ package with xlings LLVM.
set -e

LLVM_ROOT="${HOME}/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7"
if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@20.1.7 is not installed"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name    = "hello_llvm"
version = "0.1.0"

[language]
import_std = false

[toolchain]
linux = "llvm@20.1.7"
EOF

cat > src/main.cpp <<'EOF'
#include <iostream>

extern "C" int answer(void);

int main() {
    std::cout << "llvm " << answer() << "\n";
    return 0;
}
EOF

cat > src/answer.c <<'EOF'
int answer(void) {
    return 42;
}
EOF

"$MCPP" toolchain list > "$TMP/list.log" 2>&1
grep -q 'llvm 20.1.7' "$TMP/list.log" || {
    cat "$TMP/list.log"
    echo "FAIL: toolchain list did not show installed llvm 20.1.7"
    exit 1
}

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: llvm build failed"
    exit 1
}
grep -q 'Finished' "$TMP/build.log" || {
    cat "$TMP/build.log"
    echo "FAIL: build did not finish"
    exit 1
}

binary=$(find target -type f -path '*/bin/hello_llvm' | head -1)
[[ -n "$binary" && -x "$binary" ]] || {
    find target -maxdepth 5 -type f
    echo "FAIL: hello_llvm binary missing"
    exit 1
}

out=$("$binary")
[[ "$out" == "llvm 42" ]] || {
    echo "FAIL: wrong runtime output: $out"
    exit 1
}

echo "OK"
