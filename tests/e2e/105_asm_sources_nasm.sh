#!/usr/bin/env bash
# requires: gcc nasm
# NASM assembly sources (.asm) as first-class citizens: default-globbed,
# routed to the nasm_object rule with `-f` derived from the target triple,
# include dirs + %include tracked through the depfile (-MD), excluded from
# compile_commands.json, and linked into the binary.
set -e

[[ "$(uname -m)" == "x86_64" ]] || { echo "SKIP: x86-only NASM"; exit 0; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

TMP=$(mktemp -d)
COLD=$(mktemp -d)
WARM=$(mktemp -d)
trap "rm -rf '$TMP' '$COLD' '$WARM'" EXIT

cd "$TMP"
"$MCPP" new nasmix > /dev/null
cd nasmix

mkdir -p inc
cat > inc/consts.inc <<'EOF'
%define MUL_BIAS 5
EOF

cat > src/mul2.asm <<'EOF'
%include "consts.inc"
section .text
global asm_mul2
asm_mul2:
    lea rax, [rdi*2 + MUL_BIAS]
    ret
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" long asm_mul2(long a);
int main() {
    std::println("asm mul2 = {}", asm_mul2(10));
    return asm_mul2(10) == 25 ? 0 : 1;
}
EOF

# No [build].sources — the default glob must pick up the .asm file.
cat > mcpp.toml <<'EOF'
[package]
name    = "nasmix"
version = "0.1.0"

[build]
include_dirs = ["inc"]
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }

ninja_file="$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja generated"; exit 1; }

grep -q '^rule nasm_object' "$ninja_file" || {
    cat "$ninja_file"; echo "missing nasm_object rule"; exit 1; }
grep -q '^nasmfmt   = elf64' "$ninja_file" || {
    cat "$ninja_file"; echo "nasmfmt not derived as elf64"; exit 1; }
grep -qE 'build obj/mul2\.asm\.o : nasm_object .*mul2\.asm' "$ninja_file" || {
    cat "$ninja_file"; echo "mul2.asm not routed to nasm_object"; exit 1; }

# NASM units must be excluded from the CDB (clangd can't read them).
if [[ -f compile_commands.json ]]; then
    grep -q 'mul2\.asm' compile_commands.json && {
        echo "mul2.asm leaked into compile_commands.json"; exit 1; } || true
fi

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "asm mul2 = 25" ]] || { echo "unexpected output: $out"; exit 1; }

# %include dependency tracking: changing the .inc must recompile the .asm
# (via nasm -MD depfile) and change the observable result.
cat > inc/consts.inc <<'EOF'
%define MUL_BIAS 6
EOF
"$MCPP" build > rebuild.log 2>&1 || { cat rebuild.log; echo "rebuild failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "asm mul2 = 26" ]] || {
    echo "stale after %include change: $out (depfile tracking broken)"; exit 1; }

echo "OK (warm PATH nasm)"

# ─── #232 cold-env variant A: config-bootstrap error is not swallowed ──
#
# Root cause (issue #232): nasm used a bespoke, weaker provisioning path.
# An `if (cfgNasm)` guard silently swallowed a `get_cfg()` bootstrap
# failure, misreporting it as the generic "no usable nasm" message instead
# of the real cause. The fix surfaces `get_cfg()`'s error directly.
#
# To isolate THIS guard (as opposed to the toolchain-resolution gate,
# which already correctly hard-errors on a broken config), toolchain
# resolution must not itself need `get_cfg()`: `[toolchain] linux =
# "system"` is the escape hatch that resolves the compiler straight off
# $CXX/PATH, with no xlings/config involvement. A project that doesn't
# `import std;` also skips the (unrelated) std-module precompile cache,
# whose own cache-dir resolution is independent of this gate. That way
# the FIRST config-bootstrap failure the build ever surfaces is the one
# inside the nasm gate itself — deterministic, no network required.
HOSTCXX=""
for c in /usr/bin/clang++ /usr/bin/g++ clang++ g++ c++; do
    p="$(command -v "$c" 2>/dev/null)" && { HOSTCXX="$p"; break; }
done

if [[ -z "$HOSTCXX" ]]; then
    echo "SKIP: no host C++ compiler found for the cold-env (#232) variant"
else
    cd "$COLD"
    "$MCPP" new nasmix-cold > /dev/null
    cd nasmix-cold

    mkdir -p inc
    cat > inc/consts.inc <<'EOF'
%define MUL_BIAS 5
EOF
    cat > src/mul2.asm <<'EOF'
%include "consts.inc"
section .text
global asm_mul2
asm_mul2:
    lea rax, [rdi*2 + MUL_BIAS]
    ret
EOF
    cat > src/main.cpp <<'EOF'
#include <cstdio>
extern "C" long asm_mul2(long a);
int main() {
    printf("asm mul2 = %ld\n", asm_mul2(10));
    return asm_mul2(10) == 25 ? 0 : 1;
}
EOF
    cat > mcpp.toml <<'EOF'
[package]
name    = "nasmix-cold"
version = "0.1.0"

[toolchain]
linux = "system"

[build]
include_dirs = ["inc"]
EOF

    # A regular FILE where $MCPP_HOME must be a directory: config
    # bootstrap's directory-creation loop fails deterministically, no
    # network involved.
    : > "$COLD/brokenhome"

    if out=$(MCPP_HOME="$COLD/brokenhome" CXX="$HOSTCXX" "$MCPP" build 2>&1); then
        echo "cold-env (#232) build unexpectedly succeeded:"
        echo "$out"
        exit 1
    fi
    echo "$out" | grep -qi "cannot create" || {
        echo "cold-env build failed, but not with the real bootstrap error:"
        echo "$out"; exit 1; }
    if echo "$out" | grep -qi "no usable nasm"; then
        echo "REGRESSION (#232): the guard swallowed the config-bootstrap"
        echo "error and fell back to the generic 'no usable nasm' message:"
        echo "$out"
        exit 1
    fi

    echo "OK (cold-env: real bootstrap error surfaced, not swallowed)"
fi

# ─── #232 cold-env variant B: nasm absent everywhere routes through the ──
# ─── synchronous fetcher gate (never the retired bespoke message) ───────
#
# nasm hidden from both PATH (decoy, too-old version) and the sandbox
# forces the provisioning branch: `Fetcher::resolve_xpkg_path("xim:nasm@…",
# autoInstall=true, …)` — the SAME gate the compiler toolchain uses. This
# sandbox has network, but package-index/mirror conditions can be flaky in
# CI (rate limiting, mirror selection), so the assertion is deliberately
# robust to either outcome: a successful synchronous install (the common
# case) OR a real, specific failure. What #232 guarantees either way is
# that the OLD bespoke path's generic "no usable nasm ... was found or
# installable" message (which fires even when the real cause is a broken
# index/network) never appears again.
export MCPP_HOME="$WARM/home"
mkdir -p "$MCPP_HOME"
source "$SCRIPT_DIR/_inherit_toolchain.sh"
# Force a cold nasm even if the host machine already has it installed.
rm -rf "$MCPP_HOME/registry/data/xpkgs/xim-x-nasm"

DECOY="$WARM/decoy"
mkdir -p "$DECOY"
cat > "$DECOY/nasm" <<'EOF'
#!/usr/bin/env bash
# Deliberately too old (< 2.16): the version gate must reject it so the
# PATH probe falls through to sandbox lookup / provisioning, never a
# false "found".
[[ "$1" == "-v" ]] && echo "NASM version 2.10.00 compiled on Jan  1 2020"
exit 0
EOF
chmod +x "$DECOY/nasm"
export PATH="$DECOY:$PATH"

mkdir -p "$WARM/proj"
cd "$WARM/proj"
"$MCPP" new nasmix-warm > /dev/null
cd nasmix-warm

mkdir -p inc
cat > inc/consts.inc <<'EOF'
%define MUL_BIAS 5
EOF
cat > src/mul2.asm <<'EOF'
%include "consts.inc"
section .text
global asm_mul2
asm_mul2:
    lea rax, [rdi*2 + MUL_BIAS]
    ret
EOF
cat > src/main.cpp <<'EOF'
import std;
extern "C" long asm_mul2(long a);
int main() {
    std::println("asm mul2 = {}", asm_mul2(10));
    return asm_mul2(10) == 25 ? 0 : 1;
}
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "nasmix-warm"
version = "0.1.0"

[build]
include_dirs = ["inc"]
EOF

if out=$("$MCPP" build 2>&1); then
    # Synchronous provisioning succeeded: nasm must have actually landed
    # in the sandbox (proves the fetcher gate — not the decoy — supplied
    # the binary) and the program must run correctly.
    find "$MCPP_HOME/registry/data/xpkgs/xim-x-nasm" -type f -name 'nasm*' 2>/dev/null | grep -q . || {
        echo "build succeeded but nasm never landed in the sandbox (#232 gate not exercised):"
        echo "$out"; exit 1; }
    run_out="$("$MCPP" run 2>&1 | tail -1)"
    [[ "$run_out" == "asm mul2 = 25" ]] || {
        echo "cold-provisioned build produced wrong output: $run_out"; exit 1; }
    echo "OK (cold-env: nasm synchronously provisioned via the fetcher gate)"
else
    if echo "$out" | grep -qi "no usable nasm"; then
        echo "REGRESSION (#232): fell back to the old generic 'no usable"
        echo "nasm' message instead of the real provisioning failure:"
        echo "$out"
        exit 1
    fi
    echo "$out" | grep -Eqi "provisioning failed|xlings install" || {
        echo "cold-env build failed without a real provisioning error message:"
        echo "$out"; exit 1; }
    echo "NOTE: nasm cold-provisioning hit an environment/network condition"
    echo "in this sandbox (not a #232 regression) — the real error surfaced"
    echo "correctly instead of the old generic message:"
    echo "$out" | grep -i "provisioning failed"
fi

echo "OK"
