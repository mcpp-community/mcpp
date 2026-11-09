#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1062 -- `[platform.link] flags` on a platform that links through the compiler are named, not
# dropped in silence (design 2026-11-10 E8; mcpp#911 N6; SPEC-013 §4).
#
# `link.flags` are words for ld.lld itself. A platform whose `driver` is "compiler" gives them to
# no program; the build says so and names `[platform.build] ldflags`, whose words do reach the
# compiler's link.
#
# Criteria: the build of such a platform warns, naming `[platform.link] flags` and
# `[platform.build] ldflags`, and succeeds; the same platform with `driver = "lld"` does not warn.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/plat" "$t/app/src"
cat > "$t/plat/mcpp.toml" <<TOML
[package]
name    = "words-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@${LLVM_VERSION}"

[[platform.define]]
triple        = "x86_64-wordsos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
family        = "unix"
execution     = "wordsos"

[[platform.provider]]
serves = ["x86_64-wordsos"]

[platform.build]
cflags  = ["-ffreestanding", "-fno-stack-protector"]
ldflags = ["-nostdlib", "-static"]

[platform.link]
driver = "compiler"
flags  = ["-e", "_start"]

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
TOML
cat > "$t/app/mcpp.toml" <<'TOML'
[package]
name    = "words"
version = "0.1.0"

[targets.words]
kind = "bin"
main = "src/main.c"

[target.x86_64-wordsos]
platform = { path = "../plat" }
TOML
echo 'void _start(void) { for (;;) {} }' > "$t/app/src/main.c"
cd "$t/app"
out=$("$MCPP" build --target x86_64-wordsos 2>&1) || fail "the build failed" "$out"
grep -q 'states `\[platform.link\] flags`' <<<"$out" || fail "the dropped words were not named" "$out"
grep -q '`\[platform.build\] ldflags`' <<<"$out" || fail "the remedy was not named" "$out"
echo "  ok  link words a compiler link does not use are named"
sed -i 's/^driver = "compiler"$/driver = "lld"/; s/^ldflags = .*$//' "$t/plat/mcpp.toml"
rm -rf target
out=$("$MCPP" build --target x86_64-wordsos 2>&1) || fail "the lld build failed" "$out"
if grep -q 'platform.link\] flags' <<<"$out"; then fail "an lld link was warned" "$out"; fi
echo "  ok  an ld.lld link takes them without a word"
echo "PASS: 1062 link flags on a compiler link are named, not dropped"
