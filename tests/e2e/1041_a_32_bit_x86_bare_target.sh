#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1041 -- 32-bit x86 without an operating system (mcpp#911 G12, G4; SPEC-016 §3).
#
# The row `i686-none-elf` compiles for the i686 instruction set and links with ld.lld directly
# under the emulation `elf_i386`. A platform package that states `link.driver = "compiler"`
# replaces the row's direct link: the compiler driver links, with the row's flags and ld.lld named
# by path. The program enters at `_start` and ends with the 32-bit system call `exit`, so an x86_64
# Linux machine with IA-32 emulation runs it as it is.
#
# Criteria: the artifact is an ELF32 Intel 80386 executable; the link line is ld.lld's (`-m
# elf_i386`) without a platform and the compiler's (`--target=i686-none-elf ... -fuse-ld=`) with one;
# where the machine runs 32-bit programs, both artifacts exit 7.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/bsp" "$t/app/src"
cat > "$t/app/mcpp.toml" <<'EOF'
[package]
name    = "boot32"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.boot32]
kind = "bin"
main = "src/main.c"
EOF
cat > "$t/app/src/main.c" <<'EOF'
#if !defined(__i386__) || defined(__x86_64__)
#error "not a 32-bit x86 compilation"
#endif
_Static_assert(sizeof(void*) == 4, "a 32-bit pointer");
void _start(void) {
    __asm__ volatile("mov $1, %eax\n mov $7, %ebx\n int $0x80");
}
EOF
cd "$t/app"

# A 32-bit program runs here when the kernel has IA-32 emulation; elsewhere the run is skipped.
runs32() {
    [ "$(uname -m)" = x86_64 ] || return 1
    set +e; "$1"; local rc=$?; set -e
    [ "$rc" = 7 ] && return 0
    [ "$rc" = 126 ] && return 1
    fail "the artifact exited $rc, not 7"
}
check() {   # $1: what links; $2: the pattern of its link line
    local exe ninja
    exe=$(find target/i686-none-elf -path '*/bin/boot32' -type f | head -1)
    [ -n "$exe" ] || fail "no artifact under target/i686-none-elf"
    readelf -h "$exe" | grep -q 'ELF32' || fail "the artifact is not ELF32" "$(readelf -h "$exe")"
    readelf -h "$exe" | grep -q 'Intel 80386' || fail "the artifact is not for i386" "$(readelf -h "$exe")"
    ninja=$(find target/i686-none-elf -name build.ninja | head -1)
    grep -qE "^c_ldflags += $2" "$ninja" || fail "$1 does not link" "$(grep -E '^c?_?ldflags' "$ninja")"
    if runs32 "$exe"; then echo "  ok  $1 links an ELF32 i386 program that exits 7"
    else echo "  ok  $1 links an ELF32 i386 program (this machine runs no 32-bit program)"; fi
}

out=$("$MCPP" build --target i686-none-elf 2>&1) || fail "i686-none-elf did not build" "$out"
check "ld.lld under elf_i386" '-m elf_i386'

cat > "$t/bsp/mcpp.toml" <<EOF
[package]
name    = "bsp32"
version = "0.1.0"

[platform]
targets   = ["i686-none-elf"]
toolchain = "llvm@${LLVM_VERSION}"

[platform.link]
driver = "compiler"
EOF
cat >> mcpp.toml <<'EOF'

[target.i686-none-elf]
platform = { path = "../bsp" }
EOF
out=$("$MCPP" build --target i686-none-elf 2>&1) || fail "i686-none-elf with the platform did not build" "$out"
grep -q "Platform bsp32 0.1.0" <<<"$out" || fail "the platform is not reported" "$out"
check "the platform's compiler driver" '.*--target=i686-none-elf .*-fuse-ld='
echo "PASS: 1041 a 32-bit x86 bare target"
