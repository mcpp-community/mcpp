#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1049 -- a platform skeleton is made, and its statement is read as the engine reads it
# (SPEC-016 §5).
#
# `mcpp new <name> --template platform` writes a platform package that defines `<arch>-<name>` and
# the provider that serves it; `mcpp why platform` lists the identities it serves. Reading the
# platform's manifest checks the statement against the rows: a definition of a built-in row is an
# error wherever the platform is read, and a provider that serves no row is a warning of the
# platform's own build.
#
# Criteria: `mcpp why platform` lists <arch>-<name> with its provider and priority, in the platform
# and in a project that selects it; a program builds for the row; a provider serving nothing is a
# warning of the platform's build; a redefined built-in row fails the project's build and names it.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
cd "$t"
arch=$(uname -m); [ "$arch" = arm64 ] && arch=aarch64
out=$("$MCPP" new myos --template platform 2>&1) || fail "the skeleton was not made" "$out"
grep -q "mcpp why platform" <<<"$out" || fail "the next step is not named" "$out"
out=$(cd myos && "$MCPP" why platform 2>&1) || fail "the skeleton is not read" "$out"
grep -Eq "serves +$arch-myos +myos +priority 100" <<<"$out" || fail "the served row is not listed" "$out"
echo "  ok  the skeleton serves $arch-myos"

mkdir -p app/src
cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.c"]
ldflags = ["-nostdlib", "-static"]

[targets.app]
kind = "bin"
main = "src/main.c"

[target.$arch-myos]
platform = { path = "../myos" }
EOF
echo 'void _start(void) { for (;;) {} }' > app/src/main.c
out=$(cd app && "$MCPP" build --target "$arch-myos" 2>&1) || fail "a program did not build for the row" "$out"
out=$(cd app && "$MCPP" why platform --target "$arch-myos" 2>&1) || fail "the selected platform is not read" "$out"
grep -Eq "serves +$arch-myos +myos +priority 100" <<<"$out" || fail "the project's platform is not listed" "$out"
out=$(cd app && "$MCPP" why platform --target "$arch-myos" --format json 2>&1) || fail "no JSON" "$out"
grep -Eq '"identity": *"'"$arch"'-myos"' <<<"$out" || fail "the JSON lacks the served identity" "$out"
echo "  ok  a program builds for the row; the project reports the platform it selects"

cat >> myos/mcpp.toml <<'EOF'

[[platform.provider]]
name   = "nothing"
serves = ["riscv64-nosuch"]
EOF
out=$(cd myos && "$MCPP" build 2>&1) || fail "a warning ended the platform's build" "$out"
grep -q "\[\[platform.provider\]\] 'nothing' serves no row" <<<"$out" || fail "a provider serving nothing is not said" "$out"

cat >> myos/mcpp.toml <<'EOF'

[[platform.define]]
triple = "x86_64-linux-gnu"
EOF
set +e; out=$(cd app && "$MCPP" build --target "$arch-myos" 2>&1); rc=$?; set -e
[ "$rc" != 0 ] || fail "a redefined built-in row did not fail the build" "$out"
grep -q "'x86_64-linux-gnu' is a built-in row" <<<"$out" || fail "the redefinition is not named" "$out"
echo "  ok  a provider serving nothing is a warning; a redefined built-in row fails the build"
echo "PASS: 1049 a platform skeleton is made and its statement read as the engine reads it"
