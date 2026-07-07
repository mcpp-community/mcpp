#!/usr/bin/env bash
# requires: import-std-libcxx
# 86_llvm_hermetic_link.sh — hermetic-link regression fence for issue #195.
#
# Two failure modes used to be invisible:
#   - on hosts WITHOUT a system toolchain, the llvm link passed bare CRT
#     names (Scrt1.o/crti.o/crtn.o) that lld cannot open → link failure;
#   - on hosts WITH one, the driver silently resolved the HOST's CRT →
#     contaminated "green" builds that masked the first mode on CI.
# This test asserts, via a `-###` dry-run with the exact ldflags mcpp
# generated, that every CRT object and the effective dynamic linker resolve
# inside the sandbox (an xpkgs registry path) — on ANY machine, with or
# without a host toolchain.
set -e

OS="$(uname -s)"
if [[ "$OS" != Linux ]]; then
    echo "SKIP: hermetic CRT/loader model is Linux-only"
    exit 0
fi

source "$(dirname "$0")/_llvm_env.sh"
if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} is not installed"
    exit 0
fi
if [[ ! -f "$LLVM_ROOT/share/libc++/v1/std.cppm" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} has no libc++ std.cppm"
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
name    = "hermetic_hello"
version = "0.1.0"

[toolchain]
linux = "llvm@${LLVM_VERSION}"
EOF

cat > src/main.cpp <<'EOF'
import std;

int main() {
    std::println("hermetic {}", 195);
    return 0;
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: llvm build failed"
    exit 1
}

# 1. The link flags must carry the CRT discovery prefix (-B<glibc payload>).
ninja_file=$(find target -name build.ninja | head -1)
ldflags=$(grep '^ldflags' "$ninja_file" | sed 's/^ldflags *= *//')
echo "$ldflags" | grep -q ' -B' || {
    echo "ldflags: $ldflags"
    echo "FAIL: link flags carry no -B (CRT discovery prefix)"
    exit 1
}

# 2. mcpp's own hermetic check ran and cached a verdict.
[[ -f "$(dirname "$ninja_file")/.mcpp-hermetic-ok" ]] || {
    echo "FAIL: build did not record a hermetic-link verdict"
    exit 1
}

# 3. Independent -### dry-run: CRT objects + effective loader must resolve
#    to absolute paths inside an xpkgs registry (never /lib, /usr/lib, or
#    bare names). This is the assertion that fails on host contamination
#    even when the build itself "succeeds".
dry=$("$LLVM_ROOT/bin/clang++" $(echo "$ldflags" | sed 's/\$\(.\)/\1/g') \
        -### -x c++ /dev/null -o /dev/null 2>&1)

crt_tokens=$(echo "$dry" | tr ' ' '\n' | tr -d '"' \
             | grep -E '(^|/)(S|g|r|M)?crt[1in]\.o$' | grep -v clang_rt || true)
[[ -n "$crt_tokens" ]] || {
    echo "FAIL: -### output contains no CRT objects to check"
    exit 1
}
while IFS= read -r tok; do
    case "$tok" in
        */xpkgs/*) ;;  # sandbox payload — OK
        /*) echo "FAIL: CRT resolves outside the sandbox: $tok"; exit 1 ;;
        *)  echo "FAIL: bare CRT name reached the linker: $tok"; exit 1 ;;
    esac
done <<< "$crt_tokens"

# Effective dynamic linker = LAST occurrence (the driver emits its built-in
# default first; the -Wl override follows and wins).
loader=$(echo "$dry" | tr ' ' '\n' | tr -d '"' \
         | grep -o -- '--dynamic-linker=.*' | tail -1 | cut -d= -f2)
if [[ -z "$loader" ]]; then
    loader=$(echo "$dry" | tr ' ' '\n' | tr -d '"' \
             | grep -A1 -- '^-dynamic-linker$' | tail -1)
fi
[[ "$loader" == */xpkgs/* ]] || {
    echo "FAIL: effective dynamic linker outside the sandbox: '$loader'"
    exit 1
}

# 4. And the binary actually runs.
binary=$(find target -type f -path '*/bin/hermetic_hello' | head -1)
out=$("$binary")
[[ "$out" == "hermetic 195" ]] || {
    echo "FAIL: wrong runtime output: $out"
    exit 1
}

echo "OK"
