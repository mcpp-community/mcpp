#!/usr/bin/env bash
# requires: llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1059 -- a base package states the layers it holds beside its C library (design 2026-11-10 E5;
# SPEC-002 §1.3, §2.3, §2.4).
#
# A sysroot whose own `mcpp.toml` has a `[base]` table is a base package: per architecture it names
# the directory that is the sysroot (`roots`), and it states the C++ run-time and the compiler
# run-time it holds (`provides`), their headers, the libraries a link names by its form, the std
# module and the flags its module needs. One package is then the whole target side of a system,
# and nothing of the toolchain payload's own C++ run-time is used.
#
# The base here is synthetic (a C++ header and a std module file, no C library to link), so the
# test reads the plan rather than a program: the report, the words of a unit and of the link.
#
# Criteria: the report names the c++-abi and compiler-runtime layers as the base's; a C++ unit
# carries the base's C++ headers and not the payload's; the link names the base's libraries for
# a dynamic link, with -nostdlib++; an architecture the base does not hold is refused naming the
# ones it does.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/base/amd64/usr/include/c++/v1" "$t/base/amd64/usr/share/libc++/v1" "$t/base/amd64/usr/lib" \
         "$t/plat" "$t/app/src"
echo '#pragma once' > "$t/base/amd64/usr/include/c++/v1/toy_vector"
echo 'export module std;' > "$t/base/amd64/usr/share/libc++/v1/std.cppm"
cat > "$t/base/mcpp.toml" <<'TOML'
[package]
name     = "toy-sysroot"
version  = "1.0"
provides = ["mcpp:c-abi=toy-libc", "mcpp:c++-abi=libc++", "mcpp:compiler-runtime=libgcc",
            "hosted-standard-library"]

[base]
roots            = { x86_64 = "amd64" }
include-dirs     = ["usr/include/c++/v1"]
libs             = { dynamic = ["c++"], static = ["c++", "cxxrt"] }
std-module       = "usr/share/libc++/v1/std.cppm"
std-module-flags = ["-DTOY_MODULE=1"]
TOML
cat > "$t/plat/mcpp.toml" <<TOML
[package]
name    = "toy-freebsd-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@${LLVM_VERSION}"
sysroot   = { path = "../base" }

[[platform.provider]]
serves = ["x86_64-freebsd", "aarch64-freebsd"]
hosted = true

[platform.link]
driver = "compiler"

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
TOML
cat > "$t/app/mcpp.toml" <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[target.'cfg(os = "freebsd")']
platform = { path = "../plat" }
TOML
echo 'int main() { return 0; }' > "$t/app/src/main.cpp"
cd "$t/app"
out=$("$MCPP" why flags --target x86_64-freebsd 2>&1) || fail "the plan over the base package failed" "$out"
grep -qE 'c\+\+-abi +libc\+\+ +\(toy-sysroot@1.0, prebuilt' <<<"$out" || fail "the C++ layer is not the base's" "$out"
grep -qE 'compiler-runtime +libgcc +\(toy-sysroot@1.0, prebuilt' <<<"$out" || fail "the compiler run-time is not the base's" "$out"
echo "  ok  the report names the base's layers"
grep -q -- "-isystem$t/base/amd64/usr/include/c++/v1" <<<"$out" || fail "a unit lacks the base's C++ headers" "$out"
grep -E 'xim-x-llvm/[^ ]*/include/c\+\+/v1' <<<"$out" && fail "a unit carries the payload's C++ headers" "$out"
grep -q -- "--sysroot=$t/base/amd64" <<<"$out" || fail "the sysroot is not the base's root for x86_64" "$out"
echo "  ok  the base's headers and root stand where the payload's would"
grep -qE '(^| )-nostdlib\+\+( |$)' <<<"$out" && grep -qE '(^| )-lc\+\+( |$)' <<<"$out" \
    || fail "the link does not name the base's library" "$out"
echo "  ok  the link names the base's libraries"
set +e; out=$("$MCPP" why flags --target aarch64-freebsd 2>&1); rc=$?; set -e
[ "$rc" != 0 ] || fail "an architecture the base does not hold was accepted" "$out"
grep -q 'toy-sysroot@1.0 is a base for x86_64, not for aarch64' <<<"$out" || fail "the refusal does not name the base's architectures" "$out"
echo "  ok  an architecture the base does not hold is refused"
echo "PASS: 1059 a base package states the layers beside its C library"
