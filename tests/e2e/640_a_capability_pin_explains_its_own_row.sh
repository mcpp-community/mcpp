#!/usr/bin/env bash
# requires: gcc
# 640_a_capability_pin_explains_its_own_row.sh — a row whose pin is a
# capability refuses a declared toolchain that cannot emit it, and the reason
# names THAT row.
#
# THE RULE IS SHARED AND THE REASON IS NOT. `prepare.cppm` says so in its own
# comment -- "one sentence covering both would be wrong about one of them: a
# PE+musl target is not bare metal, and a reader told it is stops reading" --
# and then a third row was added without a third reason, so the sentence became
# wrong about the new one. Measured: `--target wasm32-emscripten` with a
# declared gcc was refused correctly and explained with "No gcc payload emits a
# PE with a musl C library", which is true about a different row.
#
# AND THE GATE ASKED THE WRONG QUESTION. It tested `family != Llvm`, which was
# right while every capability-pinned row pinned llvm. `wasm32-emscripten` pins
# `emsdk@6.0.9`, and emsdk normalises to the llvm family because `em++` IS
# clang -- so a declared `llvm@22.1.8` passed the gate, was never refused, and
# resolved the generic llvm payload for a target it cannot emit. Case 4 is that
# one, and it is the case a reader would not think to write.
set -e

t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail=0

# Each row's reason must appear for ITS target and for no other. The probe is a
# substring of the sentence rather than the whole of it, so rewording stays
# free while the pairing stays asserted.
check() {  # target  declared-toolchain  expected-phrase  label
    local target=$1 tc=$2 phrase=$3 label=$4
    local d="$t/$RANDOM$RANDOM"; mkdir -p "$d/src"
    printf '[package]\nname = "c"\nversion = "0.1.0"\n\n[toolchain]\nlinux = "%s"\n' "$tc" > "$d/mcpp.toml"
    printf 'int main(){return 0;}\n' > "$d/src/main.cpp"
    local out
    out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target "$target" 2>&1 ) || true
    if ! grep -q "cannot be emitted by" <<<"$out"; then
        echo "FAIL: $label — not refused at all"
        grep -m3 -E "^(error|warning)" <<<"$out" | sed 's/^/    /'
        fail=1; return
    fi
    if grep -qF "$phrase" <<<"$out"; then
        echo "  ok: $label"
    else
        echo "FAIL: $label — the refusal does not carry this row's reason"
        echo "    wanted: $phrase"
        grep -A3 "cannot be emitted by" <<<"$out" | sed 's/^/    /'
        fail=1
    fi
}

echo "== 640: the rule is shared, the reason is the row's =="

# 1-3. each row's own sentence, for a declared gcc.
check wasm32-emscripten   gcc@16.1.0 "Nothing but Emscripten emits WebAssembly" "wasm names Emscripten"
check riscv64-none-elf    gcc@16.1.0 "no per-host cross payload"                "bare metal names the cross payload"
check x86_64-windows-musl gcc@16.1.0 "PE with a musl C library"                 "PE+musl names the C library"

# 4. THE GATE. `llvm@22.1.8` is the llvm family, and so is emsdk -- so a family
#    test cannot separate them and this declaration used to pass unrefused.
check wasm32-emscripten   llvm@22.1.8 "Nothing but Emscripten emits WebAssembly" "a declared llvm is refused too"

# 5. And the sentence names the row's OWN pin rather than a fixed word: the
#    closing line used to read "The row names llvm as a capability" on every
#    row, which is wrong for the one pinned to emsdk.
d="$t/pin"; mkdir -p "$d/src"
printf '[package]\nname = "c"\nversion = "0.1.0"\n\n[toolchain]\nlinux = "gcc@16.1.0"\n' > "$d/mcpp.toml"
printf 'int main(){return 0;}\n' > "$d/src/main.cpp"
out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target wasm32-emscripten 2>&1 ) || true
if grep -q "names .emsdk@6.0.9. as a capability" <<<"$out"; then
    echo "  ok: the closing line names this row's pin"
else
    echo "FAIL: the closing line does not name emsdk@6.0.9"
    grep -A5 "cannot be emitted by" <<<"$out" | sed 's/^/    /'
    fail=1
fi

if [ "$fail" -ne 0 ]; then echo "FAIL: 640"; exit 1; fi
echo "PASS: 640"
