#!/usr/bin/env bash
# requires: elf llvm gcc musl
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1051 -- a cross target's `[target.<t>] toolchain` names the target's compiler, not the one the
# build program is compiled with.
#
# A build program is compiled and run on this machine. `[target.x86_64-linux-musl] toolchain =
# "gcc@16.1.0"` replaces the project's `[toolchain]` for the target; the build program takes the
# spec it replaced, as it takes the one a row's pin replaced (657). Measured on mcpp's own tree,
# built on Windows for x86_64-linux-musl: the build program was compiled by the musl cross gcc and
# the host could not start it (error 8, executable format error).
#
# Criteria: under `--target x86_64-linux-musl` the build program's compiler is the `[toolchain]`
# one (llvm), the target's is the section's (gcc), and the build program runs.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
[ "$(uname -m)" = x86_64 ] || { echo "SKIP: an x86_64 host"; exit 0; }
mkdir -p "$t/src" && cd "$t"
cat > mcpp.toml <<EOF
[package]
name    = "crossprog"
version = "0.1.0"

[toolchain]
default = "llvm@${LLVM_VERSION:-23.1.3}"

[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
EOF
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    std::ofstream(std::string(mcpp::out_dir()) + "/ran.txt") << "ran\n";
}
EOF
echo 'int main() { return 0; }' > src/main.cpp
out=$("$MCPP" build --target x86_64-linux-musl 2>&1) || fail "the cross build failed" "$out"
grep -q 'host toolchain for build.mcpp: llvm' <<<"$out" \
    || grep -Eq 'Resolved host toolchain for build.mcpp: clang' <<<"$out" \
    || fail "the build program is not compiled by the [toolchain] compiler" "$out"
grep -q 'x86_64-linux-musl-g++' <<<"$out" || fail "the target is not compiled by the section's gcc" "$out"
find target -name ran.txt | grep -q . || fail "the build program did not run" "$(find target -maxdepth 4 | head)"
echo "  ok  the build program takes the [toolchain] compiler; the target takes its section's"
echo "PASS: 1051 a cross target's toolchain section is not the build program's"
