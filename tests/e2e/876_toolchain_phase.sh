#!/usr/bin/env bash
# requires: llvm unix-shell symlink
# `[toolchain] default = { configure = "build.mcpp" }` (mcpp#755): the root
# build program states the build toolchain in its toolchain phase, the
# bootstrap toolchain compiles and runs the build programs, and the phase
# states nothing but the toolchain.
set -e

MCPP="${MCPP:-mcpp}"
source "$(dirname "$0")/_llvm_env.sh"
if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: no llvm payload installed ($LLVM_ROOT)"; exit 77
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1

T="$work/llvm"
mkdir -p "$T/bin"
for f in "$LLVM_ROOT"/bin/*; do
    case "$f" in *.cfg) ;; *) ln -s "$f" "$T/bin/" ;; esac
done
for d in include lib share libexec; do [ -e "$LLVM_ROOT/$d" ] && ln -s "$LLVM_ROOT/$d" "$T/$d"; done

mkdir -p "$work/app/src"
cat > "$work/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default   = { configure = "build.mcpp" }
bootstrap = "llvm@$LLVM_VERSION"
TOML
cat > "$work/app/src/main.cpp" <<'CPP'
int main() { return 0; }
CPP
write_program() {   # $1: what the toolchain phase states
    cat > "$work/app/build.mcpp" <<CPP
import std;
import mcpp;
int main() {
    if (std::string_view(mcpp::phase()) == "toolchain") {
        $1
        return 0;
    }
    mcpp::warning((std::string("build phase, compiler ") + mcpp::compiler()).c_str());
    return 0;
}
CPP
}
cd "$work/app"
fail() { echo "FAIL: $*"; echo "----"; echo "$out"; exit 1; }

write_program "mcpp::toolchain(\"path\", \"$T\"); mcpp::toolchain(\"origin\", \"build.mcpp:6\");"
rm -rf target
out="$("$MCPP" build 2>&1)" || fail "the build failed"
grep -q "Using toolchain clang .* ← $T  \[program · build.mcpp:6\]" <<<"$out" || fail "no Using line naming the build program"
grep -q "Bootstrap llvm@$LLVM_VERSION" <<<"$out" || fail "no Bootstrap line"
grep -q "build phase, compiler clang" <<<"$out" || fail "the build phase did not run"
grep -q "Finished .* · program: toolchain" <<<"$out" || fail "Finished does not summarise the source"
[[ "$(grep -c 'Resolving toolchain' <<<"$out")" -le 1 ]] || fail "the first pass narrated"

# A managed spec stated by the phase is a pinned source, and is not announced.
write_program "mcpp::toolchain(\"spec\", \"llvm@$LLVM_VERSION\");"
rm -rf target
out="$("$MCPP" build 2>&1)" || fail "a managed spec from the phase failed"
grep -q "Using toolchain" <<<"$out" && fail "a managed spec was announced as a custom source"

# A phase that states nothing, and a phase that states a flag, are refused.
write_program ""
rm -rf target
out="$("$MCPP" build 2>&1 || true)"
grep -q "stated no" <<<"$out" || fail "a phase without a statement was accepted"
write_program "mcpp::cxxflag(\"-DX\"); mcpp::toolchain(\"path\", \"$T\");"
rm -rf target
out="$("$MCPP" build 2>&1 || true)"
grep -q "stated \`mcpp:cxxflag=\` in its toolchain phase" <<<"$out" || fail "a flag in the toolchain phase was accepted"

echo "PASS: the build program states the toolchain, and the phase states nothing else"
