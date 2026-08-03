#!/usr/bin/env bash
# requires: ohos-sdk qemu-aarch64
# Linux → HarmonyOS/OpenHarmony cross: build a C++23 named-module project for
# aarch64-linux-ohos with mcpp's own clang against the platform SDK's sysroot,
# assert the artefact really is a static aarch64 OHOS ELF, and RUN it under
# qemu-aarch64. Part C of .agents/docs/2026-08-04-harmonyos-target-design.md.
#
# "Linked" has never implied "runs" (the elfpatch incident, and §6.1 of
# 2026-08-03-windows-host-linux-cross-design.md), which is why the execution
# step is the point of this test and the assertions around it are secondary.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TRIPLE=aarch64-linux-ohos

"$MCPP" new ohosdemo
cd ohosdemo

# `import std` deliberately NOT used here: a stock OpenHarmony SDK carries
# libc++ 15.0.4, which ships no std module. This test covers the shape that
# works against an unmodified SDK — see 104 for the import-std tier.
cat > src/banner.cppm <<'EOF'
module;
#include <string>
export module ohosdemo.banner;
export std::string banner() {
    return "hello from aarch64-linux-ohos, running under qemu";
}
EOF

cat > src/main.cpp <<'EOF'
#include <cstdio>
import ohosdemo.banner;
int main() {
#if !defined(__OHOS__)
#error "__OHOS__ not defined — this was not built for HarmonyOS"
#endif
    std::printf("%s\n", banner().c_str());
    std::printf("harmonyos cross named-module OK\n");
    return 0;
}
EOF

# The vocabulary pin (triple.cppm) selects an LLVM toolchain by itself, so no
# [target.*] section is written: this asserts the CONVENTION works, not just
# that an explicit override does.
"$MCPP" build --target "$TRIPLE"

BIN=$(find "target/$TRIPLE" -type f -path '*/bin/*' -name ohosdemo | head -1)
[ -n "$BIN" ] || { echo "FAIL: no artefact under target/$TRIPLE"; find target -type f | head -20; exit 1; }

echo "== file =="
file "$BIN"
# Positive `grep -q` on purpose: `! cmd | grep` is exempt from errexit and can
# never fail (build-mcpp-helper-self-containment).
file "$BIN" | grep -q "ELF 64-bit LSB"
file "$BIN" | grep -q "ARM aarch64"
file "$BIN" | grep -q "statically linked"

# Stronger than `file`'s wording and independent of it: a fully static ELF has
# no PT_INTERP at all. HarmonyOS's loader is /lib/ld-musl-aarch64.so.1, which
# no CI runner has — so an accidentally-dynamic artefact would be unrunnable
# here and this is what would catch it.
if command -v readelf &>/dev/null; then
    readelf -l "$BIN" > hdrs.txt
    if grep -q "INTERP" hdrs.txt; then
        echo "FAIL: artefact has PT_INTERP — not statically linked"
        grep -A2 "INTERP" hdrs.txt
        exit 1
    fi
fi

QEMU=$(command -v qemu-aarch64 || command -v qemu-aarch64-static)
echo "== run under $QEMU =="
OUT=$("$QEMU" "$BIN")
echo "$OUT"
echo "$OUT" | grep -q "harmonyos cross named-module OK" \
    || { echo "FAIL: artefact did not produce expected output"; exit 1; }

echo "OK: HarmonyOS cross artefact builds and executes"
