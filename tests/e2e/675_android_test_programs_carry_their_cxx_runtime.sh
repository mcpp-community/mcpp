#!/usr/bin/env bash
# requires: elf gcc android-ndk
# 675_android_test_programs_carry_their_cxx_runtime.sh -- the static C++
# runtime is located by asking the driver for the effective target, so the
# self-contained contract holds on the Android rows (#634 A6).
#
# The archive lookup searched the LLVM payload's `lib/` alone. The NDK keeps
# its archives in the sysroot per API level, the search found nothing, and the
# contract degraded:
#
#     warning: cxx_runtime: test target: this toolchain ships no
#     libc++.a/libc++abi.a; using toolchain-coupled
#
# Every test program then needed `libc++_shared.so`, and on an API 34 emulator
# every one of them stopped with `CANNOT LINK EXECUTABLE ... library
# "libc++_shared.so" not found`. The driver answers directly for the target
# the link names, API level included; the NDK's per-API `libc++.a` is a linker
# script, `INPUT(-lc++_static -lc++abi)`.
#
# No device is needed: the runner is a script that exits 0, and the criteria
# read the programs' dynamic sections and the build graph.
#
# Criteria:
#   1. test programs on `x86_64-linux-android` name no `libc++_shared.so`, and
#      the degradation warning is not printed;
#   2. their link line names the NDK sysroot's per-API `libc++.a`;
#   3. a shared library that asks for the self-contained contract hides the
#      runtime it embeds: its link names `--exclude-libs,libc++_static.a`, the
#      archive the script opens, and `std::__ndk1::to_string(int)` is not among
#      its dynamic symbols, while it still names no `libc++_shared.so`.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

readelf_bin=""
for d in "$HOME"/.xlings/data/xpkgs/xim-x-android-ndk/*/toolchains/llvm/prebuilt/*/bin \
         "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-android-ndk/*/toolchains/llvm/prebuilt/*/bin; do
    if [ -x "$d/llvm-readelf" ] && [ -x "$d/llvm-nm" ]; then readelf_bin="$d"; fi
done
[ -n "$readelf_bin" ] || fail "the android-ndk capability was detected but no llvm-readelf was found"

printf '#!/bin/sh\nexit 0\n' > "$TMP/ok-runner.sh"
chmod +x "$TMP/ok-runner.sh"

# ── 1 and 2. test programs ────────────────────────────────────────────────
"$MCPP" new droid > /dev/null
cd droid
cat > tests/strings.cpp <<'EOF'
#include <string>
#include <cstdio>
int main() { std::string s = std::to_string(42); std::puts(s.c_str()); return s == "42" ? 0 : 1; }
EOF
printf '\n[target.x86_64-linux-android]\nrunner = ["%s"]\n' "$TMP/ok-runner.sh" >> mcpp.toml
"$MCPP" test --target x86_64-linux-android > t1.log 2>&1 || fail "mcpp test on the Android row failed" t1.log
grep -q "ships no libc++.a" t1.log && fail "the contract still degraded to toolchain-coupled" t1.log
prog=$(find target/x86_64-linux-android -path '*/bin/strings' -type f | head -1)
[ -n "$prog" ] || fail "no test program was built for x86_64-linux-android" t1.log
"$readelf_bin/llvm-readelf" -d "$prog" > needed1.txt
grep -q "NEEDED" needed1.txt || fail "the test program's dynamic section could not be read" needed1.txt
grep -q "libc++_shared.so" needed1.txt && fail "the test program still needs libc++_shared.so" needed1.txt
grep -q "sysroot/usr/lib/x86_64-linux-android/[0-9]*/libc++.a" target/x86_64-linux-android/*/build.ninja \
    || fail "the link line does not name the sysroot's per-API libc++.a" t1.log
echo "Android test programs carry their C++ runtime OK"

# ── 3. a self-contained shared library hides what it embeds ───────────────
cd "$TMP"
mkdir -p fw/src
cat > fw/mcpp.toml <<'EOF'
[package]
name    = "fw"
version = "0.1.0"

[build]
cxx_runtime = { shared = "self-contained" }

[targets.fw]
kind = "shared"
EOF
cat > fw/src/fw.cppm <<'EOF'
module;
#include <string>
export module fw;
export std::string fw_name() { return "fw-" + std::to_string(7); }
EOF
cd fw
"$MCPP" build --target x86_64-linux-android > b3.log 2>&1 || fail "the shared library did not build" b3.log
so=$(find target/x86_64-linux-android -name 'libfw.so' -type f | head -1)
[ -n "$so" ] || fail "no libfw.so was built" b3.log
grep -q "exclude-libs,libc++_static.a" target/x86_64-linux-android/*/build.ninja \
    || fail "the link does not hide the archive the linker script opens" b3.log
"$readelf_bin/llvm-readelf" -d "$so" > needed3.txt
grep -q "libc++_shared.so" needed3.txt && fail "the self-contained shared library needs libc++_shared.so" needed3.txt
"$readelf_bin/llvm-nm" -D --defined-only -C "$so" > dyn3.txt
grep -q "std::__ndk1::to_string(int)" dyn3.txt \
    && fail "the shared library exports the C++ runtime it embeds" dyn3.txt
echo "a self-contained Android shared library hides the embedded runtime OK"

echo "PASS: 675_android_test_programs_carry_their_cxx_runtime"
