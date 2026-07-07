#!/usr/bin/env bash
# requires: import-std-libcxx
# 47_llvm_atomic_link.sh — a program using a 16-byte std::atomic lowers to
# __atomic_*_16 libcalls that live in libatomic. Compiler drivers don't
# auto-link libatomic, so without mcpp injecting `-latomic` this fails at link
# with `undefined symbol __atomic_load_16`. Verifies the fix links + runs, and
# that a program NOT using atomics gets no libatomic dependency (--as-needed).
set -e

OS="$(uname -s)"
if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    echo "SKIP: libatomic/-latomic path is Linux-only"
    exit 0
fi
if [[ "$OS" == Darwin* ]]; then
    echo "SKIP: libatomic is a GNU/Linux runtime; not applicable on macOS"
    exit 0
fi

source "$(dirname "$0")/_llvm_env.sh"

if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} is not installed"
    exit 0
fi
# The fix only emits -latomic when a link-resolvable libatomic is present in
# the toolchain (self-guarding). If a slimmed package does not bundle it, the
# genuine-atomic case is out of scope here.
if [[ ! -e "$LLVM_ROOT/lib/x86_64-unknown-linux-gnu/libatomic.so" \
   && ! -e "$LLVM_ROOT/lib/x86_64-unknown-linux-gnu/libatomic.a" ]]; then
    echo "SKIP: llvm@${LLVM_VERSION} package ships no link-resolvable libatomic"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<EOF
[package]
name    = "atomic_link"
version = "0.1.0"

[toolchain]
linux = "llvm@${LLVM_VERSION}"
EOF

# 16-byte atomic → __atomic_load_16 / __atomic_compare_exchange_16 libcalls.
cat > src/main.cpp <<'EOF'
#include <atomic>
#include <cstdio>
struct alignas(16) Big { long long a, b; };
std::atomic<Big> g{{0, 0}};
int main() {
    g.store(Big{1, 2});
    Big r = g.load();
    Big e{1, 2}, d{9, 9};
    bool ok = g.compare_exchange_strong(e, d);
    std::printf("atomic %lld %lld %d\n", r.a, r.b, (int)ok);
    return 0;
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: 16-byte atomic build failed (libatomic not linked?)"
    exit 1
}

binary=$(find target -type f -path '*/bin/atomic_link' | head -1)
[[ -n "$binary" && -x "$binary" ]] || {
    echo "FAIL: atomic_link binary missing"
    exit 1
}

out=$("$binary")
[[ "$out" == "atomic 1 2 1" ]] || {
    echo "FAIL: wrong runtime output: $out"
    exit 1
}

# The genuine-atomic binary must actually pull libatomic (used → kept).
if command -v readelf >/dev/null 2>&1; then
    readelf -d "$binary" 2>/dev/null | grep -q 'libatomic\.so' || {
        echo "FAIL: atomic binary missing libatomic NEEDED"
        exit 1
    }
fi

# A program that does NOT use atomics must get no libatomic dependency,
# proving --as-needed drops the spurious link.
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("noatomic\n"); return 0; }
EOF
"$MCPP" build --no-cache > "$TMP/build2.log" 2>&1 || {
    cat "$TMP/build2.log"
    echo "FAIL: non-atomic rebuild failed"
    exit 1
}
binary=$(find target -type f -path '*/bin/atomic_link' | head -1)
if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$binary" 2>/dev/null | grep -q 'libatomic\.so'; then
        echo "FAIL: non-atomic binary should not depend on libatomic (--as-needed)"
        exit 1
    fi
fi

echo "OK"
