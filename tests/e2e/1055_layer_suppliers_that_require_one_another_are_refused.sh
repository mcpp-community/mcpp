#!/usr/bin/env bash
# requires: gcc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1055 -- the suppliers of the target side may not require one another in a circle (design
# 2026-11-10 E14; SPEC-002 §3.2).
#
# A C library built on the kernel interface requires it; an implementation of the kernel interface
# written above a C library requires the C library. When both supply the graph's layers, neither is
# beneath the other: the composition used to build, and the program called itself at run time.
#
# Criteria: the build is refused at resolution, naming both suppliers and what each requires, with
# the code [layer-cycle] in its message, as refusals a measurement reads carry it; without the C library's requirement it resolves.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/kimpl/src" "$t/clib/src" "$t/app/src"
cat > "$t/kimpl/mcpp.toml" <<'TOML'
[package]
name     = "kimpl"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=toykernel"]
requires = ["mcpp:c-abi"]

[targets.kimpl]
kind = "lib"
TOML
echo 'int toy_kernel(void) { return 1; }' > "$t/kimpl/src/k.c"
cat > "$t/clib/mcpp.toml" <<'TOML'
[package]
name     = "clib"
version  = "0.1.0"
provides = ["mcpp:c-abi=toylibc"]
requires = ["mcpp:kernel-abi=toykernel"]

[targets.clib]
kind = "lib"
TOML
echo 'int toy_libc(void) { return 2; }' > "$t/clib/src/c.c"
cat > "$t/app/mcpp.toml" <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
kimpl = { path = "../kimpl" }
clib  = { path = "../clib" }

[targets.app]
kind = "bin"
main = "src/main.c"
TOML
echo 'int main(void) { return 0; }' > "$t/app/src/main.c"
cd "$t/app"
set +e; out=$("$MCPP" build --message-format json 2>&1); rc=$?; set -e
[ "$rc" != 0 ] || fail "a circle of layer requirements built" "$out"
grep -q 'require one another in a circle' <<<"$out" || fail "the refusal names no circle" "$out"
grep -q '`kimpl@0.1.0` supplies the kernel-abi and requires the c-abi' <<<"$out" \
    || fail "the kernel interface's supplier is not named" "$out"
grep -q '`clib@0.1.0` supplies the c-abi and requires the kernel-abi' <<<"$out" \
    || fail "the C library's supplier is not named" "$out"
grep -q '\[layer-cycle\]' <<<"$out" || fail "the refusal carries no layer-cycle code" "$out"
echo "  ok  the circle is refused, naming both suppliers"

sed -i '/^requires = \["mcpp:kernel-abi=toykernel"\]$/d' "$t/clib/mcpp.toml"
out=$("$MCPP" build 2>&1 || true)
grep -q 'require one another in a circle' <<<"$out" && fail "a one-way requirement was refused" "$out"
echo "  ok  a requirement in one direction is no circle"
echo "PASS: 1055 layer suppliers that require one another are refused"
