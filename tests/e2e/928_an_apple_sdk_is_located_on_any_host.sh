#!/usr/bin/env bash
# requires: unix-shell
# Apple's own convention for naming an SDK, `SDKROOT`, is read on every host
# (design 2026-10-23 §4.4, D18, S12). An SDK a developer produced from their
# own Xcode on Linux is located the way one in Xcode is, and its slices decide
# which rows it can serve (D16). Nothing is downloaded: mcpp supports the SDK
# and does not distribute it.
#
# Linux only: on macOS `xcrun` answers first, which is the other arm, covered
# by the iOS workflow.
set -e

MCPP="${MCPP:-mcpp}"
[[ "$(uname -s)" == Linux ]] || { echo "SKIP: the non-Apple arm"; exit 0; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1 MCPP_NO_AUTO_INSTALL=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

sdk="$work/iPhoneSimulator18.5.sdk"
mkdir -p "$sdk/usr/include" "$sdk/usr/lib"
printf '{"CanonicalName":"iphonesimulator18.5","Version":"18.5"}\n' > "$sdk/SDKSettings.json"
printf -- '--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-ios-simulator ]\ninstall-name: /usr/lib/libSystem.B.dylib\n' \
    > "$sdk/usr/lib/libSystem.tbd"
mkdir -p "$work/app/src" && cd "$work/app"
printf 'int main() { return 0; }\n' > src/main.cpp
printf '[package]\nname = "app"\nversion = "0.1.0"\n' > mcpp.toml

# Without it, the refusal says where it looked and how to provide one.
out="$(env -u SDKROOT "$MCPP" build --target aarch64-ios-sim 2>&1 || true)"
grep -q "needs the iphonesimulator SDK, which this machine does not provide" <<<"$out" || fail "no refusal without an SDK"
grep -q "SDKROOT (unset)" <<<"$out" || fail "the refusal does not say SDKROOT was looked at"
grep -q "xtool" <<<"$out" || fail "the refusal does not say how to produce one off macOS"

# With it, the SDK gate opens: whatever stops the build next, it is not that.
out="$(SDKROOT="$sdk" "$MCPP" build --target aarch64-ios-sim 2>&1 || true)"
grep -q "which this machine does not provide" <<<"$out" && fail "SDKROOT was not honoured off macOS"

# The slice decides the row: this SDK has no x86_64 simulator slice.
out="$(SDKROOT="$sdk" "$MCPP" build --target x86_64-ios-sim 2>&1 || true)"
grep -q "carries no x86_64-ios-simulator slice" <<<"$out" || fail "a missing slice was not named"

# A version is matched, and a mismatch says what was found.
printf '[package]\nname = "app"\nversion = "0.1.0"\n[target.aarch64-ios-sim]\nsysroot = "iphonesimulator@17.0"\n' > mcpp.toml
out="$(SDKROOT="$sdk" "$MCPP" build --target aarch64-ios-sim 2>&1 || true)"
grep -q "is version 18.5, and iphonesimulator@17.0 was asked for" <<<"$out" || fail "a version mismatch was not named"

# And the family names the platform: an iphoneos SDK is not a simulator's.
printf '[package]\nname = "app"\nversion = "0.1.0"\n[target.aarch64-ios-sim]\nsysroot = "iphoneos@system"\n' > mcpp.toml
out="$("$MCPP" build --target aarch64-ios-sim 2>&1 || true)"
grep -q "describes another platform" <<<"$out" || fail "a locator of another platform was accepted"

echo "PASS: an Apple SDK is located by SDKROOT on any host, and its slices decide the row"
