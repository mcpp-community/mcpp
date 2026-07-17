#!/usr/bin/env bash
# requires: gcc nasm
# NASM assembly sources (.asm) as first-class citizens: default-globbed,
# routed to the nasm_object rule with `-f` derived from the target triple,
# include dirs + %include tracked through the depfile (-MD), excluded from
# compile_commands.json, and linked into the binary.
set -e

[[ "$(uname -m)" == "x86_64" ]] || { echo "SKIP: x86-only NASM"; exit 0; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

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

ninja_file="$(find target -name build.ninja | head -1)"
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

echo "OK"
