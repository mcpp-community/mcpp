#!/usr/bin/env bash
# The mcpp-index side, inside the sandbox: a project that names only
# `compat.cuda-driver` resolves it from the PUBLISHED index, installs it, and
# the artifact reaches the host driver through mcpp's private loader.
set -uo pipefail
S=/home/speak/.xlings/data/xpkgs/xim-x-mcpp/2026.9.5.2/bin/mcpp
fails=0
fail() { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails+1)); }

d=$(mktemp -d); mkdir -p "$d/src"; cd "$d" || exit 1
cat > mcpp.toml <<'TOML'
[package]
name = "idx"
version = "0.1.0"

[language]
standard = "c++23"
modules = true
import_std = true

[dependencies.compat]
cuda-driver = "2026.09.05"

[build]
sources = ["src/*.cpp"]

[targets.idx]
kind = "bin"
main = "src/main.cpp"
TOML
cat > src/main.cpp <<'CPP'
#include <dlfcn.h>
#include <cstdio>
int main() {
    void* h = dlopen("libcuda.so.1", RTLD_LAZY);
    std::printf("%s\n", h ? "driver reachable" : dlerror());
    return 0;
}
CPP

out=$("$S" run 2>&1)
printf '%s\n' "$out" | grep -q "compat.cuda-driver" \
    || fail "compat.cuda-driver did not resolve from the published index"
printf '%s\n' "$out" | grep -q "driver reachable" \
    || fail "the artifact did not reach the host driver ($(printf '%s' "$out" | tail -2 | tr '\n' ' '))"
printf '%s\n' "$out" | grep -q "driver reachable" \
    && echo "ok: compat.cuda-driver resolves from the published index and the artifact reaches the driver"

[ "$fails" -eq 0 ] && { echo "INDEX SIDE OK"; exit 0; }
echo "$fails ASSERTION(S) FAILED"; exit 1
