#!/usr/bin/env bash
# requires: gcc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1056 -- a package that requires the kernel-abi layer reads the interfaces its provider states
# (design 2026-11-10 E13; docs/30).
#
# The resolved kernel-abi provider states `[kernel-abi] provides-interfaces`. The build program of a
# package that requires the layer receives the list as MCPP_KERNEL_ABI_INTERFACES, joined by ';'
# (`mcpp::kernel_abi_interfaces()`), and a runner of images binds exactly those. A package that does
# not require the layer receives the variable empty, as it does when the provider states no list.
#
# Criteria: the requiring package's program reads `openkal.abort;openkal.fs`; the other package's
# program reads an empty value; with the provider's list removed the requiring program reads empty.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/impl/src" "$t/runner/src" "$t/other/src" "$t/app/src"
cat > "$t/impl/mcpp.toml" <<'TOML'
[package]
name     = "impl"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=toykernel"]

[kernel-abi]
provides-interfaces = ["openkal.abort", "openkal.fs"]

[targets.impl]
kind = "lib"
TOML
echo 'int toy_kernel(void) { return 1; }' > "$t/impl/src/k.c"
program() {   # $1: package, $2: requires line or empty
cat > "$t/$1/mcpp.toml" <<TOML
[package]
name     = "$1"
version  = "0.1.0"
$2

[targets.$1]
kind = "lib"
TOML
echo "int $1_fn(void) { return 2; }" > "$t/$1/src/$1.c"
cat > "$t/$1/build.mcpp" <<CPP
import std;
import mcpp;
int main() { std::println("mcpp:warning=$1 sees [{}]", mcpp::kernel_abi_interfaces()); }
CPP
}
program runner 'requires = ["mcpp:kernel-abi=toykernel"]'
program other  ''
cat > "$t/app/mcpp.toml" <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
impl   = { path = "../impl" }
runner = { path = "../runner" }
other  = { path = "../other" }

[targets.app]
kind = "bin"
main = "src/main.c"
TOML
echo 'int main(void) { return 0; }' > "$t/app/src/main.c"
cd "$t/app"
out=$("$MCPP" build 2>&1 || true)
grep -q 'runner sees \[openkal.abort;openkal.fs\]' <<<"$out" || fail "the requiring package did not read the list" "$out"
grep -q 'other sees \[\]' <<<"$out" || fail "a package that does not require the layer read something" "$out"
echo "  ok  the requiring package reads the provider's interfaces; another package reads nothing"

sed -i '/^\[kernel-abi\]$/,/^provides-interfaces/d' "$t/impl/mcpp.toml"
out=$("$MCPP" build 2>&1 || true)
grep -q 'runner sees \[\]' <<<"$out" || fail "an unstated list was not empty" "$out"
echo "  ok  a provider that states no list gives an empty value (unknown)"
echo "PASS: 1056 a runner reads the interfaces its implementation provides"
