#!/usr/bin/env bash
# 40_llvm_bmi_cache.sh — Clang BMI cache reuse for dependency packages.
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

# mcpplibs packages live in a separate registry namespace; inherit it so the
# index lookup for mcpplibs.cmdline succeeds in the isolated MCPP_HOME.
USER_MCPP="${HOME}/.mcpp"
if [[ -d "$USER_MCPP/registry/data/mcpplibs" ]]; then
    mkdir -p "$MCPP_HOME/registry/data"
    [[ -e "$MCPP_HOME/registry/data/mcpplibs" ]] \
        || ln -sf "$USER_MCPP/registry/data/mcpplibs" "$MCPP_HOME/registry/data/mcpplibs"
fi

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name    = "llvm_cache"
version = "0.1.0"
[toolchain]
linux = "llvm@20.1.7"
[dependencies]
"mcpplibs.cmdline" = "0.0.1"
EOF

cat > src/main.cpp <<'EOF'
import std;
import mcpplibs.cmdline;
int main() {
    std::println("cache test ok");
    return 0;
}
EOF

# First build — should compile the dependency
out1=$("$MCPP" build --no-cache 2>&1)
echo "$out1" | grep -q "Compiling.*mcpplibs.cmdline" || {
    # It's OK if it says "Cached" because global cache may exist
    echo "$out1" | grep -q "Cached.*mcpplibs.cmdline" || {
        echo "FAIL: mcpplibs.cmdline not mentioned in first build: $out1"
        exit 1
    }
}

# Second build (clean target, keep BMI cache) — dependency should be cached
rm -rf target
out2=$("$MCPP" build 2>&1)
echo "$out2" | grep -q "Cached.*mcpplibs.cmdline" || {
    echo "FAIL: mcpplibs.cmdline not cached on second build: $out2"
    exit 1
}

echo "OK"
