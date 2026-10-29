#!/usr/bin/env bash
# #857 probe: inside the Android 14 emulator, under `run-as com.termux`, every
# step of openkal-linux's start of a program, and mcpp itself. Prints, never asserts.
set -uo pipefail
WORK="${RUNNER_TEMP:-/tmp}/p857"
S="${GITHUB_STEP_SUMMARY:-/dev/null}"
A=/data/data/com.termux/files
adb wait-for-device
adb install -r "$WORK/termux.apk" >/dev/null
adb shell am start -n com.termux/com.termux.app.TermuxActivity >/dev/null
for _ in $(seq 1 60); do adb shell run-as com.termux ls "$A/usr/bin/sh" >/dev/null 2>&1 && break; sleep 5; done
for f in mcpp.tar.gz exec_probe; do
    adb push "$WORK/$f" /data/local/tmp/ >/dev/null
    adb shell chmod 644 "/data/local/tmp/$f"
    adb shell run-as com.termux cp "/data/local/tmp/$f" "$A/$f"
done
adb shell run-as com.termux chmod 755 "$A/exec_probe"
adb logcat -c
run() { adb shell run-as com.termux "$@" 2>&1 | tr -d '\r'; }
{
echo "SELinux: $(adb shell getenforce | tr -d '\r'); context: $(run cat /proc/self/attr/current | tr -d '\0')"
echo "kernel: $(adb shell uname -a | tr -d '\r')"
run mkdir -p "$A/home/dist"
run "$A/usr/bin/tar" -xzf "$A/mcpp.tar.gz" -C "$A/home/dist"
D=$(run "$A/usr/bin/sh" -c "ls -d $A/home/dist/mcpp-*")
echo "release: $D"
echo; echo "### the bundled xlings (static ELF)"
run "$A/exec_probe" "$D/registry/bin" xlings --version
echo; echo "### mcpp itself"
run "$A/exec_probe" "$D/bin" mcpp --version
echo; echo "### Termux's sh (bionic, dynamic)"
run "$A/exec_probe" "$A/usr/bin" sh -c true
echo; echo "### the probe copying itself"
run cp "$A/exec_probe" "$A/home/exec_probe_copy"
run "$A/exec_probe" "$A/home" exec_probe_copy
echo; echo "### mcpp 2026.10.29.1 under run-as (the failing case)"
run "$A/usr/bin/env" HOME="$A/home" TMPDIR="$A/home/tmp" PATH="$A/usr/bin" "$D/bin/mcpp" self env 2>&1 | head -20
echo; echo "### avc denials"
adb logcat -d -b all 2>/dev/null | tr -d '\r' | grep -i 'avc' | tail -40
} | tee "$WORK/probe.log"
{ echo '### #857 probe'; echo '```'; cat "$WORK/probe.log"; echo '```'; } >> "$S"
