#!/usr/bin/env bash
# requires: gcc elf
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 804_a_path_host_tool_builds_with_its_chosen_toolchain.sh — mcpp#710.
#
# A host tool is built by one compiler, chosen once and recorded in the tool
# store's key: `MCPP_TOOLCHAIN` when set, else the tool package's own
# `[toolchain]` (its workspace's, for a member), else the compiler the
# consumer compiles its build programs with. The member case has a unit test
# (HostToolToolchain.AMemberToolReadsItsWorkspaceToolchain). This test covers
# a tool reached through a plain path dependency, which belongs to no
# workspace:
#   1. a tool package that names no toolchain is built by the consumer's
#      build-program compiler (llvm ${LLVM_VERSION} here), not by the global default
#      (gcc on Linux), which is what the sub-build used to resolve for itself;
#   2. a tool package that names its own toolchain is built by it
#      (gcc 16.1.0) while the consumer keeps llvm.
# The compiler that produced the tool is read from its `.comment` section.
set -e
candidate_seed="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.github/tools" && pwd)/seed_native_xim_index.py"
if [[ "${MCPP_E2E_804_LLVM_HOST_ONLY:-0}" == 1 ]]; then
    [[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || {
        echo "FAIL: LLVM-only host-helper admission requires native Linux ARM64"; exit 1; }
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# An isolated home, so neither the user's default toolchain nor a stale tool
# store entry decides the outcome. The registry is shared: toolchains are
# expensive to install.
export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi
if [[ -n "${MCPP_NATIVE_XIM_INDEX:-}" ]]; then
    python3 "$candidate_seed" "$MCPP_HOME" "$MCPP_NATIVE_XIM_INDEX"
fi
unset MCPP_TOOLCHAIN

mkdir -p toolpkg/src app/src
write_tool() {   # $1 = the body of the tool's [toolchain] table, empty for none
    cat > toolpkg/mcpp.toml <<'TOML'
[package]
name    = "toolpkg"
version = "0.1.0"

[targets.stamp]
kind = "bin"
main = "src/stamp.cpp"
TOML
    if [[ -n "$1" ]]; then
        printf '\n[toolchain]\n%s\n' "$1" >> toolpkg/mcpp.toml
    fi
}
cat > toolpkg/src/stamp.cpp <<'CPP'
int main() { return 0; }
CPP

cat > app/mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default = "llvm@${LLVM_VERSION}"

[dependencies]
toolpkg = { path = "../toolpkg", tools = ["stamp"] }
TOML
cat > app/src/main.cpp <<'CPP'
int main() {}
CPP
cat > app/build.mcpp <<'CPP'
#include <string>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("toolpkg", "stamp");
    if (!tool || !*tool) return 1;
    mcpp::warning((std::string("TOOL=") + tool).c_str());
}
CPP

tool_path() { sed -n 's/.*TOOL=//p' "$1" | tail -1; }

# ── 1 ── no toolchain of its own: the consumer's build-program compiler
write_tool ""
(cd app && "$MCPP" build > ../b1.log 2>&1) || { cat b1.log; echo "FAIL: 1: build failed"; exit 1; }
t="$(tool_path b1.log)"
[[ -x "$t" ]] || { cat b1.log; echo "FAIL: 1: no tool binary at '$t'"; exit 1; }
readelf -p .comment "$t" > c1.txt
grep -q 'clang version 23\.1\.3' c1.txt || {
    cat c1.txt; echo "FAIL: 1: the tool was not built by the consumer's llvm ${LLVM_VERSION}"; exit 1; }
echo "ok: 1"

# ARM64 publishes LLVM GNU, while native GCC GNU 16.1.0 is unavailable. This
# explicit admission mode proves only the path host-helper LLVM case; the
# ordinary test still exercises both compiler families below.
if [[ "${MCPP_E2E_804_LLVM_HOST_ONLY:-0}" == 1 ]]; then
    echo "PASS: 804 native LLVM path host helper (LLVM case only)"
    exit 0
fi

# ── 2 ── its own toolchain: that one, whatever the consumer uses
write_tool 'default = "gcc@16.1.0"'
rm -rf app/target
(cd app && "$MCPP" build > ../b2.log 2>&1) || { cat b2.log; echo "FAIL: 2: build failed"; exit 1; }
t="$(tool_path b2.log)"
[[ -x "$t" ]] || { cat b2.log; echo "FAIL: 2: no tool binary at '$t'"; exit 1; }
readelf -p .comment "$t" > c2.txt
grep -qE 'GCC: \(.*\) 16\.1\.0' c2.txt || {
    cat c2.txt; echo "FAIL: 2: the tool's own [toolchain] was not used"; exit 1; }
if grep -q 'clang version' c2.txt; then
    cat c2.txt; echo "FAIL: 2: the tool was built by the consumer's clang"; exit 1
fi
echo "ok: 2"

echo "PASS: 804_a_path_host_tool_builds_with_its_chosen_toolchain"
