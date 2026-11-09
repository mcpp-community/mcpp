#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1061 -- an artifact for another system is packed without running it (design 2026-11-10 E7;
# mcpp#911 N8; docs/10).
#
# A row whose C library a platform describes and whose programs run on another system: the
# vendored closure is read from the program's DT_NEEDED rather than from the host's loader (`ldd`
# of a FreeBSD program exited 127). A library this build produced travels with the program; every
# other name is the target system's. `--mode static` links the program statically on a named row
# too, and the file is asked before it is packed under that name.
#
# Criteria: `mcpp pack` (vendored) packs the program and its library and runs no host loader;
# `--mode static` packs a program that names neither a loader nor a shared library.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/plat" "$t/app/src" "$t/awaylib/src"
cat > "$t/plat/mcpp.toml" <<TOML
[package]
name    = "away-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@${LLVM_VERSION}"

[[platform.define]]
triple        = "x86_64-awayos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
family        = "unix"
execution     = "awayos"

[[platform.provider]]
serves = ["x86_64-awayos"]

[platform.build]
cflags  = ["-ffreestanding", "-fno-stack-protector"]
ldflags = ["-nostdlib"]

[platform.link]
driver = "compiler"

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
TOML
cat > "$t/awaylib/mcpp.toml" <<'TOML'
[package]
name    = "awaylib"
version = "0.1.0"

[targets.awaylib]
kind = "shared"
TOML
echo 'int away_value(void) { return 7; }' > "$t/awaylib/src/away.c"
cat > "$t/app/mcpp.toml" <<'TOML'
[package]
name    = "away"
version = "0.1.0"

[dependencies]
awaylib = { path = "../awaylib" }

[targets.away]
kind = "bin"
main = "src/main.c"

[target.x86_64-awayos]
platform = { path = "../plat" }
TOML
cat > "$t/app/src/main.c" <<'C'
int away_value(void);
void _start(void) { (void)away_value(); for (;;) {} }
C
cd "$t/app"
out=$("$MCPP" build --target x86_64-awayos 2>&1) || fail "the row did not build" "$out"
exe=$(find target/x86_64-awayos -path '*/bin/away' -type f | head -1)
[ -n "$exe" ] || fail "no program" "$out"
readelf -d "$exe" | grep -q NEEDED || fail "the program names no shared library, so this test tests nothing" "$(readelf -d "$exe")"
out=$("$MCPP" pack --target x86_64-awayos --all-programs 2>&1) || fail "the vendored pack failed" "$out"
grep -qi 'ldd failed' <<<"$out" && fail "the host's ldd was run on the artifact" "$out"
listing=$(tar tzf target/dist/away-0.1.0-x86_64-awayos.tar.gz)
grep -q '/bin/away$' <<<"$listing" || fail "the program is not packed" "$listing"
grep -qE 'libawaylib[^/]*\.so' <<<"$listing" || fail "the library the build produced does not travel" "$listing"
echo "  ok  the closure is read from the file, and the build's library travels"
# `--mode static` is a static program on a named row as well (N8: the program was packed dynamic
# under the `-static` name); the library, a static archive here, links into it.
sed -i 's/^kind = "shared"$/kind = "lib"/' "$t/awaylib/mcpp.toml"
out=$("$MCPP" pack --target x86_64-awayos --mode static 2>&1) || fail "the static pack failed" "$out"
static_exe=$(tar tzf target/dist/away-0.1.0-x86_64-awayos-static.tar.gz | grep '/bin/away$' | head -1)
tar xzf target/dist/away-0.1.0-x86_64-awayos-static.tar.gz -C "$t" "$static_exe"
if readelf -d "$t/$static_exe" 2>/dev/null | grep -q NEEDED || readelf -l "$t/$static_exe" | grep -q INTERP; then
    fail "--mode static packed a dynamic program" "$(readelf -dl "$t/$static_exe" | head -20)"
fi
echo "  ok  --mode static packs a static program"
echo "PASS: 1061 an artifact for another system is packed without running it"
