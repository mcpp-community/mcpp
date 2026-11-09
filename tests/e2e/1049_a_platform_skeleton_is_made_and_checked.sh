#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1049 -- a platform skeleton is made and checked (SPEC-016 §2, §3).
#
# `mcpp new <name> --template platform` writes a platform package that defines `<arch>-<name>` and
# the provider that serves it; `mcpp check platform` reads it as the engine does and lists the
# identities it serves. A row that names a built-in row is refused, and a provider that serves
# nothing is said.
#
# Criteria: the skeleton checks clean and serves x86_64-<name> (on x86_64); a program builds for
# the row; a redefined built-in row ends the check with 1; a provider serving nothing is a warning.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
cd "$t"
arch=$(uname -m); [ "$arch" = arm64 ] && arch=aarch64
out=$("$MCPP" new myos --template platform 2>&1) || fail "the skeleton was not made" "$out"
out=$(cd myos && "$MCPP" check platform 2>&1) || fail "the skeleton does not check" "$out"
grep -Eq "serves $arch-myos +myos +priority 100" <<<"$out" || fail "the check does not list the served row" "$out"
echo "  ok  the skeleton checks and serves $arch-myos"

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
echo "  ok  a program builds for the platform's row"

cat >> myos/mcpp.toml <<'EOF'

[[platform.define]]
triple = "x86_64-linux-gnu"

[[platform.provider]]
name   = "nothing"
serves = ["riscv64-nosuch"]
EOF
set +e; out=$(cd myos && "$MCPP" check platform 2>&1); rc=$?; set -e
[ "$rc" = 1 ] || fail "a redefined built-in row did not fail the check (exit $rc)" "$out"
grep -q "'x86_64-linux-gnu' is a built-in row" <<<"$out" || fail "the redefinition is not named" "$out"
grep -q "the provider 'nothing' serves no row" <<<"$out" || fail "a provider serving nothing is not said" "$out"
echo "  ok  a redefined built-in row fails the check; a provider serving nothing is said"
echo "PASS: 1049 a platform skeleton is made and checked"
