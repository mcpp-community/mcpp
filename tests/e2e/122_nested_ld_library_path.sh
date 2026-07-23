#!/usr/bin/env bash
# requires:
# Inherited LD_LIBRARY_PATH pointing at mcpp's private glibc payload must not
# crash mcpp's own children (ninja/gcc). Reproduces the nested-mcpp chain:
# outer `mcpp run` poisons the env for its child; the child spawns mcpp again;
# the inner mcpp's tools then load a mismatched libc and segfault in the
# dynamic linker (trace signature: bare `__vdso_time` line).
set -e

GLIBC_LIB=$(ls -d "$HOME"/.mcpp/registry/data/xpkgs/xim-x-glibc/*/lib64 2>/dev/null | head -1)
if [[ -z "$GLIBC_LIB" ]]; then
    echo "SKIP: no private glibc payload installed"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "nest"
version = "0.1.0"
standard = "c++23"
EOF
echo 'int main() { return 0; }' > tests/t.cpp

out=$(LD_LIBRARY_PATH="$GLIBC_LIB" "$MCPP" test 2>&1) \
    || { echo "poisoned run failed: $out"; exit 1; }
[[ "$out" == *"t ... ok"* ]] || { echo "test did not pass under poison: $out"; exit 1; }
echo OK
