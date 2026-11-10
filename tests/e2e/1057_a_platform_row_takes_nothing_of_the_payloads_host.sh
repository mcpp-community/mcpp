#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1057 -- a row a platform defines takes its machine description and nothing of the toolchain
# payload's own target side (design 2026-11-10 E1; mcpp#911 N2, N3, N7; SPEC-016 §2.1, §5).
#
# The engine's predicates read the rows a platform defines as they read the built-in ones: the
# row's `march` and `link-emulation` reach the compile and an `ld.lld` link, and the payload's libc++
# headers and runtime directories -- built for the payload's own host -- reach neither the compile
# nor the link of a row whose C library the platform describes.
#
# Criteria: `mcpp why flags` names `-march=i386` and `-m elf_i386` as the row's; no word of the
# compile or the link names the payload's `include/c++/v1`, a `lib/<host triple>` directory or the
# gcc runtime; the build succeeds and its program is ELF32.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/plat" "$t/app/src"
cat > "$t/plat/mcpp.toml" <<TOML
[package]
name    = "toy32-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@${LLVM_VERSION}"

[[platform.define]]
triple         = "i386-toy32"
clang-triple   = "i386-unknown-none-elf"
object-format  = "elf"
family         = "unix"
execution      = "toy32"
march          = "i386"
link-emulation = "elf_i386"

[[platform.provider]]
serves = ["i386-toy32"]

[platform.build]
cflags = ["-ffreestanding", "-fno-pic"]

[platform.link]
driver = "lld"
flags  = ["-e", "_start"]

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
TOML
cat > "$t/app/mcpp.toml" <<'TOML'
[package]
name    = "toy32"
version = "0.1.0"

[targets.toy32]
kind = "bin"
main = "src/main.c"

[target.i386-toy32]
platform = { path = "../plat" }
TOML
cat > "$t/app/src/main.c" <<'C'
_Static_assert(sizeof(void*) == 4, "a 32-bit pointer");
void _start(void) { for (;;) {} }
C
cd "$t/app"
out=$("$MCPP" build --target i386-toy32 2>&1) || fail "the row did not build" "$out"
flags=$("$MCPP" why flags --target i386-toy32 2>&1) || fail "why flags failed" "$flags"
grep -qE '^ *row +-march=i386 ' <<<"$flags" || fail "the row's march is not the row's word" "$flags"
grep -qE '^ *row +-m elf_i386 ' <<<"$flags" || fail "the row's emulation is not on the link" "$flags"
echo "  ok  the row's machine description reaches the compile and the link"
if grep -E 'include/c\+\+/v1|/lib/[a-z0-9_]+-unknown-linux-gnu|gcc-runtime' <<<"$flags" | grep -v '^ *compiler '; then
    fail "the payload's own target side reached a platform row" "$flags"
fi
echo "  ok  nothing of the payload's host side reaches the row"
exe=$(find target/i386-toy32 -path '*/bin/toy32' -type f | head -1)
[ -n "$exe" ] || fail "no program"
readelf -h "$exe" | grep -q ELF32 || fail "the program is not ELF32" "$(readelf -h "$exe")"
echo "PASS: 1057 a platform row takes nothing of the payload's host"
