#!/usr/bin/env bash
# requires: ohos-sdk qemu-aarch64 ohos-libcxx
# The `import std` tier of the HarmonyOS target: with a libc++ built FOR
# aarch64-linux-ohos supplied through $MCPP_OHOS_LIBCXX, mcpp builds the std
# module for the target and `import std;` works — the same experience mcpp
# gives on every other platform.
#
# Split from 103 because the two tiers fail differently and a combined test
# could not say which one broke. 103 is the floor (stock SDK, named modules
# only); this is the upgrade, and the capability gate is what keeps it from
# being a false red on a machine that only has the SDK.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TRIPLE=aarch64-linux-ohos

"$MCPP" new ohosstd
cd ohosstd

cat > src/main.cpp <<'EOF'
import std;
int main() {
    std::vector<std::string> parts{"import", "std", "on", "HarmonyOS"};
    std::string joined;
    for (auto const& p : parts) { if (!joined.empty()) joined += ' '; joined += p; }
    std::println("{}", joined);
    std::println("total={}", std::accumulate(parts.begin(), parts.end(), std::size_t{0},
                                             [](std::size_t a, auto const& s) { return a + s.size(); }));
    return 0;
}
EOF

"$MCPP" build --target "$TRIPLE"

BIN=$(find "target/$TRIPLE" -type f -path '*/bin/*' -name ohosstd | head -1)
[ -n "$BIN" ] || { echo "FAIL: no artefact under target/$TRIPLE"; exit 1; }

file "$BIN" | grep -q "ARM aarch64"
file "$BIN" | grep -q "statically linked"

QEMU=$(command -v qemu-aarch64 || command -v qemu-aarch64-static)
OUT=$("$QEMU" "$BIN")
echo "$OUT"
echo "$OUT" | grep -q "import std on HarmonyOS" \
    || { echo "FAIL: unexpected output"; exit 1; }
echo "$OUT" | grep -q "total=20" \
    || { echo "FAIL: std::accumulate over the target's std module misbehaved"; exit 1; }

echo "OK: import std works on HarmonyOS"
