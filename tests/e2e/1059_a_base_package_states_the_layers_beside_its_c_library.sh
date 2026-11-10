#!/usr/bin/env bash
# requires: llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1059 -- a base package states the layers it holds beside its C library (design 2026-11-10 E5;
# SPEC-002 §2.5).
#
# A sysroot whose root carries a package manifest is a base package, read as any package is: its
# `provides` answers its layers and its `[build]` keys state them -- `include_dirs` the C++
# library's headers, `ldflags` its libraries, `std-module` and `std-module-flags` its std module --
# with paths relative to the package root, which is the sysroot. One package serves one identity;
# a platform gives each identity its own base through each provider's `sysroot`.
#
# The bases here are synthetic (a C++ header and a std module file, no C library to link), so the
# test reads the plan rather than a program: the report, the words of a unit and of the link.
#
# Criteria: the report names the c++-abi and compiler-runtime layers as the base's; a C++ unit
# carries the base's C++ headers and not the payload's; the link names the base's libraries, with
# -nostdlib++; each architecture's build takes the base its provider names.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/plat" "$t/app/src"
for arch in x86_64 aarch64; do
    b="$t/$arch-toy-sysroot"
    mkdir -p "$b/usr/include/c++/v1" "$b/usr/share/libc++/v1" "$b/usr/lib"
    echo '#pragma once' > "$b/usr/include/c++/v1/toy_vector"
    echo 'export module std;' > "$b/usr/share/libc++/v1/std.cppm"
    cat > "$b/mcpp.toml" <<TOML
[package]
name     = "$arch-toy-sysroot"
version  = "1.0"
provides = ["mcpp:c-abi=toy-libc", "mcpp:c++-abi=libc++", "mcpp:compiler-runtime=libgcc",
            "hosted-standard-library"]

[build]
include_dirs     = ["usr/include/c++/v1"]
ldflags          = ["-lc++", "-lcxxrt"]
std-module       = "usr/share/libc++/v1/std.cppm"
std-module-flags = ["-DTOY_MODULE=1"]
TOML
done
cat > "$t/plat/mcpp.toml" <<TOML
[package]
name    = "toy-freebsd-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@${LLVM_VERSION}"

[[platform.provider]]
serves  = ["x86_64-freebsd"]
sysroot = { path = "../x86_64-toy-sysroot" }
hosted  = true

[[platform.provider]]
serves  = ["aarch64-freebsd"]
sysroot = { path = "../aarch64-toy-sysroot" }
hosted  = true

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
grep -qE 'c\+\+-abi +libc\+\+ +\(x86_64-toy-sysroot@1.0, prebuilt' <<<"$out" || fail "the C++ layer is not the base's" "$out"
grep -qE 'compiler-runtime +libgcc +\(x86_64-toy-sysroot@1.0, prebuilt' <<<"$out" || fail "the compiler run-time is not the base's" "$out"
echo "  ok  the report names the base's layers"
grep -q -- "-isystem$t/x86_64-toy-sysroot/usr/include/c++/v1" <<<"$out" || fail "a unit lacks the base's C++ headers" "$out"
grep -E 'xim-x-llvm/[^ ]*/include/c\+\+/v1' <<<"$out" && fail "a unit carries the payload's C++ headers" "$out"
grep -q -- "--sysroot=$t/x86_64-toy-sysroot" <<<"$out" || fail "the sysroot is not the base's root" "$out"
echo "  ok  the base's headers and root stand where the payload's would"
grep -qE '(^| )-nostdlib\+\+( |$)' <<<"$out" && grep -qE '(^| )-lc\+\+( |$)' <<<"$out" \
    && grep -qE '(^| )-lcxxrt( |$)' <<<"$out" || fail "the link does not name the base's libraries" "$out"
echo "  ok  the link names the base's libraries"
out=$("$MCPP" why flags --target aarch64-freebsd 2>&1) || fail "the aarch64 plan failed" "$out"
grep -q -- "--sysroot=$t/aarch64-toy-sysroot" <<<"$out" || fail "aarch64 does not take its own base" "$out"
grep -qE 'c\+\+-abi +libc\+\+ +\(aarch64-toy-sysroot@1.0, prebuilt' <<<"$out" || fail "aarch64's C++ layer is not its base's" "$out"
echo "  ok  each identity takes the base its provider names"
out=$("$MCPP" why flags --target x86_64-freebsd-musl 2>&1) && fail "the musl row was served by a platform that does not serve it" "$out"
grep -q 'serves x86_64-freebsd, aarch64-freebsd and not x86_64-freebsd-musl' <<<"$out" \
    || fail "the refusal does not name what the platform serves" "$out"
echo "  ok  a row the chosen platform does not serve is refused, naming the rows it serves"
echo "PASS: 1059 a base package states the layers beside its C library"
