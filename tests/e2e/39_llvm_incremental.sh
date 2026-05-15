#!/usr/bin/env bash
# 39_llvm_incremental.sh — Clang per-file incremental rebuild via clang-scan-deps dyndep.
set -e

LLVM_ROOT="${HOME}/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7"
if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@20.1.7 is not installed"
    exit 0
fi
if [[ ! -f "$LLVM_ROOT/share/libc++/v1/std.cppm" ]]; then
    echo "SKIP: xlings llvm@20.1.7 has no libc++ std.cppm"
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
name    = "llvm_inc"
version = "0.1.0"
[toolchain]
linux = "llvm@20.1.7"
EOF

cat > src/greet.cppm <<'EOF'
export module llvm_inc.greet;
import std;
export std::string greet() { return "hello"; }
EOF

cat > src/main.cpp <<'EOF'
import std;
import llvm_inc.greet;
int main() { std::println("{}", greet()); }
EOF

# First build (full)
"$MCPP" build --no-cache > /dev/null 2>&1

# Second build (no-op) should be fast
out=$("$MCPP" build 2>&1)
[[ "$out" == *"Finished"* ]] || { echo "FAIL: no-op rebuild failed: $out"; exit 1; }

# Touch greet.cppm only; rebuild should NOT recompile main.cpp
sleep 1
touch src/greet.cppm
out=$("$MCPP" build --verbose 2>&1)
echo "$out" | grep -q "greet.cppm" || { echo "FAIL: greet.cppm not rebuilt: $out"; exit 1; }
# main.cpp should NOT appear in ninja output (it wasn't touched)
if echo "$out" | grep -q "MOD obj/main.o\|OBJ obj/main.o"; then
    # Note: main.o MAY be relinked but should NOT be recompiled from source
    # Check specifically for compilation (not linking)
    if echo "$out" | grep -q "\-c.*main.cpp"; then
        echo "FAIL: main.cpp was recompiled when only greet.cppm changed"
        exit 1
    fi
fi

echo "OK"
