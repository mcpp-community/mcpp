#!/usr/bin/env bash
# requires: android-ndk android-device
# 658 -- `mcpp run --target aarch64-linux-android` on a real device (#622).
#
# The Android rows were `verified` through qemu-user on a system image's
# bionic. This script is the device reading: the program is pushed, executed
# and its exit status returned by `adb-run`, the session program
# `xim:android-platform-tools` ships (37.0.1-3 and later), named as this
# target's runner and provisioned by the same manifest. The engine hands the
# runner the link output and nothing else; the session is the package's.
#
# The capability probe in run_all.sh selects an arm64 device that is not an
# emulator and exports its serial as E2E_ANDROID_SERIAL; adb-run addresses it
# through ANDROID_SERIAL, which is adb's own selector.
#
#   1. a program prints its marker on the device and mcpp run exits 0;
#   2. a program that exits 7 makes mcpp run exit non-zero -- the status
#      crosses the session, which is what distinguishes a runner from a
#      launcher that reports its own success.
set -e
TARGET=aarch64-linux-android
: "${E2E_ANDROID_SERIAL:?run_all.sh exports the serial of the selected device}"
export ANDROID_SERIAL="$E2E_ANDROID_SERIAL"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/p/src"
cat > "$TMP/p/mcpp.toml" <<TOML
[package]
name = "ondevice"
version = "0.1.0"

[targets.ondevice]
kind = "bin"
main = "src/main.cpp"

[targets.exits7]
kind = "bin"
main = "src/exits7.cpp"

[target.$TARGET]
min_api_level = 24
runner = ["adb-run"]

[target.$TARGET.xlings.workspace]
"xim:android-platform-tools" = ">=37.0.1-3"
TOML
cat > "$TMP/p/src/main.cpp" <<'CPP'
#include <cstdio>
#include <string>
int main() { std::string s = "1-2-" + std::to_string(3); std::puts(s.c_str()); return 0; }
CPP
cat > "$TMP/p/src/exits7.cpp" <<'CPP'
int main() { return 7; }
CPP

cd "$TMP/p"
"$MCPP" run ondevice --target "$TARGET" > run.log 2>&1 \
    || fail "mcpp run on the device failed" run.log
grep -qx '1-2-3' run.log || fail "the program's marker did not come back from the device" run.log
echo "1. the marker printed on device $ANDROID_SERIAL OK"

if "$MCPP" run exits7 --target "$TARGET" > run7.log 2>&1; then
    fail "a program exiting 7 on the device was reported as success" run7.log
fi
echo "2. the exit status crossed the session OK"

echo "PASS: 658_a_program_runs_on_an_attached_android_device"
