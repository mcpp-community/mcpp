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

# 3b. AND IT HAPPENED A SECOND TIME, with Android. The row gained a pin, became
#     a capability row, and the reason chain still had three arms -- so the
#     refusal explained it with the PE+musl sentence, the identical wrong answer
#     case 1 was written for.
check aarch64-linux-android gcc@16.1.0 "An Android target needs bionic" "android names bionic"
check x86_64-linux-android  gcc@16.1.0 "An Android target needs bionic" "android names bionic (x86_64)"

# 4. THE GATE. `llvm@22.1.8` is the llvm family, and so is emsdk -- so a family
#    test cannot separate them and this declaration used to pass unrefused.
check wasm32-emscripten   llvm@22.1.8 "Nothing but Emscripten emits WebAssembly" "a declared llvm is refused too"
# The NDK normalises to the llvm family for the same reason, so the same hole
# would have existed for Android. A declared `llvm@22.1.8` names a real
# compiler that emits aarch64 ELF perfectly well -- what it cannot supply is
# bionic, which is why this row is a capability at all.
check aarch64-linux-android llvm@22.1.8 "An Android target needs bionic" "a declared llvm is refused for android too"

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

# 6. EXHAUSTIVE, BECAUSE ADDING AN ARM IS WHAT KEEPS FAILING.
#
# Cases 1-5 each name a row a reader thought of. The defect both times was a
# row NOBODY thought of falling into a final `else` written as another row's
# answer, and no per-row test can catch that. So: take every pinned row from
# the engine's own vocabulary, declare a toolchain that is not its pin, and
# assert the PE+musl sentence appears for exactly one of them.
#
# The denominator comes from `toolchain list`, so a row added tomorrow is in it
# without this file being edited.
echo "== 640/6: the PE+musl sentence belongs to exactly one row =="
pinned=$( "$MCPP" toolchain list --format json 2>/dev/null \
          | tr ',' '\n' | grep -o '"target": *"[^"]*"' | sed 's/.*: *"//;s/"//' )
[ -n "$pinned" ] || { echo "FAIL: toolchain list named no targets"; exit 1; }
peMusl=0; examined=0
for target in $pinned; do
    d="$t/x-$target"; mkdir -p "$d/src"
    printf '[package]\nname = "c"\nversion = "0.1.0"\n\n[toolchain]\nlinux = "gcc@16.1.0"\n' > "$d/mcpp.toml"
    printf 'int main(){return 0;}\n' > "$d/src/main.cpp"
    out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target "$target" 2>&1 ) || true
    grep -q "cannot be emitted by" <<<"$out" || continue   # not a capability row
    examined=$((examined + 1))
    if grep -qF "PE with a musl C library" <<<"$out"; then
        peMusl=$((peMusl + 1))
        if [ "$target" != "x86_64-windows-musl" ]; then
            echo "FAIL: $target is explained with the PE+musl sentence"
            fail=1
        fi
    fi
done
echo "  examined $examined capability-pinned rows of $(wc -w <<<"$pinned") targets"
if [ "$examined" -lt 4 ]; then
    echo "FAIL: only $examined capability rows were reached — the enumeration is too small to be evidence"
    fail=1
fi
if [ "$peMusl" -ne 1 ]; then
    echo "FAIL: the PE+musl sentence was printed for $peMusl rows, expected exactly 1"
    fail=1
else
    echo "  ok: exactly one row is explained by the PE+musl sentence"
fi

if [ "$fail" -ne 0 ]; then echo "FAIL: 640"; exit 1; fi
echo "PASS: 640"
