#!/usr/bin/env bash
# requires: llvm unix-shell
# 947 -- the symbol a freestanding image is entered at (design 2026-10-26
# §24.4, mcpp#845).
#
# An image with no C runtime is entered where its starter jumps: the ELF entry
# a loader or an emulator reads. It came only from the linker script's
# `ENTRY()` or the linker's default `_start`, and a symbol nothing defined was a
# warning -- "cannot find entry symbol; not setting start address" -- so the
# image linked and began at address 0.
#
# Criteria (aarch64-none-elf, the zero-libc tier, no emulator needed):
#   A. `start = "_reset"` makes `_reset` the image's entry over the script's
#      `ENTRY(_start)`, and the build says so on an `Entry` line.
#   B. A `start` that nothing defines fails the link, and the note names the
#      statement.
#   C. A board package's `mcpp:link-start` enters every image over it.
#   D. The program and the board stating different symbols is refused, naming
#      both.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

READELF=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm" -name llvm-readelf 2>/dev/null | head -1)
NM=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm" -name llvm-nm 2>/dev/null | head -1)

# board: a linker script and a build program that states the entry symbol.
mkdir -p "$TMP/board/src"
cat > "$TMP/board/mcpp.toml" <<'TOML'
[package]
name    = "board"
version = "0.1.0"

[targets.board]
kind = "lib"
TOML
printf 'export module board;\nexport namespace board { inline int id() { return 7; } }\n' > "$TMP/board/src/board.cppm"
cat > "$TMP/board/link.ld" <<'LD'
ENTRY(_start)
SECTIONS {
    . = 0x40000000;
    .text   : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    . = ALIGN(16); . = . + 0x4000; __stack_top = .;
}
LD
board_program() {   # board_program <statement in main>
    cat > "$TMP/board/build.mcpp" <<CPP
import mcpp;
import std;
int main() {
    mcpp::link_script(std::format("{}/link.ld", MCPP_PACKAGE_ROOT_FOR_TEST).c_str());
    $1
    return 0;
}
CPP
    sed -i "s|MCPP_PACKAGE_ROOT_FOR_TEST|\"$TMP/board\"|" "$TMP/board/build.mcpp"
}

mkdir -p "$TMP/kern/src"
cat > "$TMP/kern/src/main.cpp" <<'CPP'
import board;
extern "C" void kmain() {
    volatile unsigned int* const uart = reinterpret_cast<unsigned int*>(0x09000000);
    *uart = static_cast<unsigned int>(board::id());
    for (;;) { asm volatile("wfi"); }
}
asm(".section .text.entry,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "  b _start\n"
    ".globl _reset\n"
    "_reset:\n"
    "  ldr x30, =__stack_top\n"
    "  mov sp, x30\n"
    "  bl kmain\n"
    "1: b 1b\n");
CPP
kern() {   # kern <extra target keys>
    cat > "$TMP/kern/mcpp.toml" <<TOML
[package]
name    = "kern"
version = "0.1.0"

[build]
target = "aarch64-none-elf"

[dependencies]
board = { path = "../board" }

[targets.kern]
kind = "bin"
main = "src/main.cpp"
$1
TOML
}
entry_of() {   # the image's ELF entry, as hex without leading zeros
    "$READELF" -h "$1" | awk '/Entry point address/ { print $4 }'
}
address_of() {   # address_of <image> <symbol>
    printf '0x%x\n' "0x$("$NM" "$1" | awk -v s="$2" '$3 == s { print $1 }')"
}

cd "$TMP/kern"

# A
board_program ''
kern 'start = "_reset"'
"$MCPP" build > a.log 2>&1 || fail "A: the image did not build" a.log
ELF=$(find target/aarch64-none-elf -type f -name kern | head -1)
[ -n "$ELF" ] || fail "A: no image" a.log
[ "$(entry_of "$ELF")" = "$(address_of "$ELF" _reset)" ] \
    || fail "A: the image is not entered at _reset ($(entry_of "$ELF"))" a.log
grep -q 'Entry .*kern  _reset  (\[targets.kern\] start = "_reset")' a.log \
    || fail "A: no Entry line naming the statement" a.log
echo "ok: the stated symbol is the image's entry"

# B
kern 'start = "_missing"'
"$MCPP" build > b.log 2>&1 && fail "B: an image entered at an undefined symbol linked" b.log
grep -q "the image is entered at \`_missing\`, which nothing this program links defines" b.log \
    || fail "B: the note does not name the symbol" b.log
grep -q 'It was named by \[targets.kern\] start = "_missing"' b.log \
    || fail "B: the note does not name the statement" b.log
echo "ok: an entry symbol nothing defines fails the link"

# C
board_program 'mcpp::link_start("_reset");'
kern ''
"$MCPP" build > c.log 2>&1 || fail "C: the image over the board did not build" c.log
ELF=$(find target/aarch64-none-elf -type f -name kern | head -1)
[ "$(entry_of "$ELF")" = "$(address_of "$ELF" _reset)" ] \
    || fail "C: the board's link-start did not enter the image" c.log
echo "ok: a board's link-start enters every image over it"

# D
kern 'start = "_start"'
"$MCPP" build > d.log 2>&1 && fail "D: two entry symbols were accepted" d.log
grep -q "target 'kern' is entered at one symbol, and the statements disagree" d.log \
    || fail "D: the conflict is not stated" d.log
grep -q '\[targets.kern\] start = "_start"' d.log && grep -q 'mcpp:link-start=_reset. from the build program of' d.log \
    || fail "D: the conflict does not name both statements" d.log
echo "ok: two statements of the entry symbol are refused"

echo "PASS: 947 an image is entered at one stated symbol"
