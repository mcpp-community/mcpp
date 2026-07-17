#!/usr/bin/env bash
# requires: gcc
# GAS assembly sources (.S) as first-class citizens: globbed by the DEFAULT
# sources glob, routed to the asm_object rule (C driver, asm-safe flags),
# preprocessed (cpp macros work), linked into the binary, and covered on
# BOTH build and test paths (the 0.0.94 feature-sources lesson: always
# compare the two).
set -e

# x86_64 assembly — host-aware skip elsewhere (aarch64 self-host e2e).
[[ "$(uname -m)" == "x86_64" ]] || { echo "SKIP: x86_64-only asm"; exit 0; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new gasmix > /dev/null
cd gasmix

# .S is preprocessed: ASM_BIAS proves cpp ran before the assembler.
cat > src/add3.S <<'EOF'
#define ASM_BIAS 7
    .text
    .globl asm_add3
#if defined(__ELF__)
    .type asm_add3, @function
#endif
asm_add3:
    leaq (%rdi,%rsi), %rax
    addq %rdx, %rax
    addq $ASM_BIAS, %rax
    ret
#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" long asm_add3(long a, long b, long c);
int main() {
    std::println("asm add3 = {}", asm_add3(1, 2, 3));
    return asm_add3(1, 2, 3) == 13 ? 0 : 1;
}
EOF

# NOTE: no [build].sources — the default glob must pick up the .S file.
cat > mcpp.toml <<'EOF'
[package]
name    = "gasmix"
version = "0.1.0"
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }

ninja_file="$(find target -name build.ninja | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja generated"; exit 1; }

grep -q '^rule asm_object' "$ninja_file" || {
    cat "$ninja_file"; echo "missing asm_object rule"; exit 1; }
# Routed to asm_object with the extension-qualified object name (.S.o).
grep -qE 'build obj/add3\.S\.o : asm_object .*add3\.S' "$ninja_file" || {
    cat "$ninja_file"; echo "add3.S not routed to asm_object"; exit 1; }
# The global asm flag string must not smuggle in a C/C++ standard.
grep -E '^asmflags  =' "$ninja_file" | grep -q -- '-std=' && {
    echo "asmflags carries a -std flag"; exit 1; } || true

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "asm add3 = 13" ]] || { echo "unexpected output: $out"; exit 1; }

# Incremental: touching the .S recompiles and relinks successfully.
touch src/add3.S
"$MCPP" build > rebuild.log 2>&1 || { cat rebuild.log; echo "rebuild failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "asm add3 = 13" ]] || { echo "post-rebuild output: $out"; exit 1; }

# Test path: the asm object must link into test binaries too.
mkdir -p tests
cat > tests/test_asm.cpp <<'EOF'
extern "C" long asm_add3(long a, long b, long c);
int main() { return asm_add3(2, 3, 4) == 16 ? 0 : 1; }
EOF
"$MCPP" test > test.log 2>&1 || { cat test.log; echo "mcpp test failed"; exit 1; }

echo "OK"
