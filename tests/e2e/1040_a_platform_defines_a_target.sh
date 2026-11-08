#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1040 -- a platform defines a target the engine's table does not have (mcpp#911 G1; SPEC-016 §2).
#
# A platform package states `[[platform.define]]` rows: the triple, the triple its compiler is
# given, the object format, the family and the system the artifact runs on. The parser then accepts
# the row's system, `cfg(os = ...)` answers by it, the output directory is named after it, and the
# platform's provider serves it. The row here is a toy system whose artifact is a static Linux ELF,
# so the host runs it.
#
# Criteria: `--target x86_64-toyos` with the platform builds into target/x86_64-toyos, the
# `cfg(os = "toyos")` table applies, the program runs (`mcpp run`) and exits as written; without
# the platform the same request is an unknown target.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/toy" "$t/app/src"
cat > "$t/toy/mcpp.toml" <<EOF
[package]
name    = "toy-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@${LLVM_VERSION}"

[[platform.define]]
triple        = "x86_64-toyos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
family        = "unix"
execution     = "linux"

[[platform.provider]]
serves = ["x86_64-toyos"]

[platform.build]
cflags = ["-ffreestanding", "-fno-stack-protector", "-fno-pic"]

[platform.link]
driver = "compiler"

[platform.layers]
"c-abi" = false
"c++-abi" = false
"compiler-runtime" = false
"kernel-abi" = false
EOF
cat > "$t/app/mcpp.toml" <<'EOF'
[package]
name    = "toyapp"
version = "0.1.0"

[build]
sources = ["src/*.c"]
ldflags = ["-nostdlib", "-static", "-no-pie"]

[targets.toyapp]
kind = "bin"
main = "src/main.c"

[target.x86_64-toyos]
platform = { path = "../toy" }

[target.'cfg(os = "toyos")'.build]
cflags = ["-DTOY_SYSTEM=1"]
EOF
cat > "$t/app/src/main.c" <<'EOF'
#ifndef TOY_SYSTEM
#error "cfg(os = \"toyos\") did not apply"
#endif
void _start(void) {
    __asm__ volatile("mov $60, %eax\n mov $7, %edi\n syscall");
}
EOF
cd "$t/app"

out=$("$MCPP" build --target x86_64-toyos 2>&1) || fail "the platform's row did not build" "$out"
grep -q "Target x86_64-toyos → x86_64-unknown-linux-gnu" <<<"$out" || fail "the report names no clang triple of the row" "$out"
grep -q "Platform toy-platform 0.1.0" <<<"$out" || fail "the platform is not reported" "$out"
exe=$(find target/x86_64-toyos -path '*/bin/toyapp' -type f | head -1)
[ -n "$exe" ] || fail "no artifact under target/x86_64-toyos" "$(ls -R target | head -30)"
if readelf -l "$exe" | grep -q INTERP; then fail "the artifact names a program interpreter: the host's C library leaked in"; fi
set +e; "$exe"; rc=$?; set -e
[ "$rc" = 7 ] || fail "the artifact exited $rc, not 7"
echo "  ok  the row builds into target/x86_64-toyos, cfg(os = \"toyos\") applies, and it runs"

set +e; run=$("$MCPP" run --target x86_64-toyos 2>&1); rc=$?; set -e
[ "$rc" = 7 ] || fail "mcpp run ended $rc, not 7" "$run"
echo "  ok  mcpp run runs it on the system its row names (exit 7)"

# A row whose artifacts run on another system: the build does not judge their runtime closure by
# this machine's loader, and says so.
sed -i 's/^execution     = "linux"/execution     = "toyos"/' "$t/toy/mcpp.toml"
out=$("$MCPP" build --target x86_64-toyos 2>&1) || fail "a row for another system did not build" "$out"
grep -q "the artifacts are for toyos, not this machine" <<<"$out" || fail "the build judged a foreign artifact by this machine" "$out"
echo "  ok  an artifact for another system is built and its closure is not judged here"

# The platform names its programs; a build program that names them for the host does not change
# the platform's suffix, and the build says so (mcpp#911 B2).
sed -i 's/^driver = "compiler"$/driver = "compiler"\nexe    = "{name}.toy"/' "$t/toy/mcpp.toml"
cat > build.mcpp <<'EOF'
import std;
int main() { std::println("mcpp:artifact-name=exe:{{name}}"); }
EOF
out=$("$MCPP" build --target x86_64-toyos 2>&1) || fail "the build with the program's name did not succeed" "$out"
grep -q 'the platform toy-platform 0.1.0 names it `{name}.toy`: the platform.s name stands' <<<"$out" \
    || fail "the conflict of names is not reported" "$out"
[ -n "$(find target/x86_64-toyos -path '*/bin/toyapp.toy' -type f | head -1)" ] \
    || fail "the program is not named as the platform names it" "$(find target/x86_64-toyos -path '*/bin/*' | head)"
rm build.mcpp
echo "  ok  the platform's name of a program stands over a build program's"

sed -i '/^\[target.x86_64-toyos\]/,/^platform/d' mcpp.toml
out=$("$MCPP" build --target x86_64-toyos 2>&1 || true)
grep -q "unknown target 'x86_64-toyos'" <<<"$out" || fail "without the platform the request was not an unknown target" "$out"
echo "  ok  without the platform the request is an unknown target"
echo "PASS: 1040 a platform defines a target"
