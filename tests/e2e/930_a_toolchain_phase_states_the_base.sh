#!/usr/bin/env bash
# requires: llvm elf unix-shell
# A toolchain phase states the target base beside a managed compiler (design
# 2026-10-23 §3.5, S9): `spec` and `sysroot` together, the program class --
# the seam through which a plugin that LOCATES a base hands it to the engine.
# And `spec` with a key that only describes a toolchain named by path is
# refused by name; it used to be dropped without a word.
set -e

MCPP="${MCPP:-mcpp}"
source "$(dirname "$0")/_llvm_env.sh"
source "$(dirname "$0")/_sysroot_fixture.sh"
[[ -x "$LLVM_ROOT/bin/clang++" ]] || { echo "SKIP: no llvm payload installed ($LLVM_ROOT)"; exit 0; }
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo "SKIP: the fixture sysroot is x86_64 Linux"; exit 0; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

make_glibc_sysroot "$work/sr" || { echo "SKIP: no glibc payload to build a sysroot from"; exit 0; }
mkdir -p "$work/app/src"
cd "$work/app"
cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default   = { configure = "build.mcpp" }
bootstrap = "llvm@$LLVM_VERSION"
TOML
printf '#include <gnu/libc-version.h>\nimport std;\nint main() { std::println("ran {}", gnu_get_libc_version()); }\n' > src/main.cpp
phase() {
    cat > build.mcpp <<CPP
import std;
import mcpp;
int main() {
    if (std::string_view(mcpp::phase()) == "toolchain") { $1 return 0; }
    return 0;
}
CPP
}

phase "mcpp::toolchain(\"spec\", \"llvm@$LLVM_VERSION\"); mcpp::toolchain(\"sysroot\", \"$work/sr\"); mcpp::toolchain(\"origin\", \"build.mcpp:4\");"
rm -rf target
out="$("$MCPP" run 2>&1)" || fail "spec + sysroot from the phase failed"
grep -q "Using sysroot glibc .* ← $work/sr  \[program · build.mcpp:4\]" <<<"$out" \
    || fail "the base stated by the phase is not reported as the program's"
a=$(ls target/*/*/bin/app | head -1)
readelf -l "$a" | grep -q "interpreter: $work/sr/" || fail "the artifact does not load the phase's base"

phase "mcpp::toolchain(\"spec\", \"llvm@$LLVM_VERSION\"); mcpp::toolchain(\"prefix\", \"x-\");"
rm -rf target
out="$("$MCPP" build 2>&1 || true)"
grep -q "stated \`spec\` with prefix" <<<"$out" || fail "spec + prefix was not refused by name"

echo "PASS: a toolchain phase states the base; spec with a path-only key is refused"
