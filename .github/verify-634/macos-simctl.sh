#!/usr/bin/env bash
# M5: which `simctl launch` spelling never loses an application's output, and
# what exit status each one returns. Two applications: one that prints a
# marker and exits 7 at once, one that sleeps a second first. Written for
# the macOS system bash (3.2): no associative arrays.
set +e
set -u
. "$(dirname "$0")/common.sh"

N=${M5_RUNS:-20}
W="$RUNNER_TEMP/m5"
rm -rf "$W"; mkdir -p "$W"
cd "$W" || exit 1

SDK=$(xcrun --sdk iphonesimulator --show-sdk-path 2>/dev/null)
if [ -z "$SDK" ]; then reading m5.skip "no iphonesimulator SDK on this runner"; exit 0; fi

# A timeout with no coreutils: perl's alarm, then exec.
with_timeout() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }

make_app() {  # make_app <bundle-id> <sleep 0|1>
    local id="$1" app="$W/$1.app"
    mkdir -p "$app"
    cat > "$W/$1.c" <<C
#include <stdio.h>
#include <unistd.h>
int main(void) { if ($2) sleep(1); puts("M634-MARKER"); fflush(stdout); return 7; }
C
    xcrun --sdk iphonesimulator clang -target arm64-apple-ios17.0-simulator \
        -isysroot "$SDK" "$W/$1.c" -o "$app/probe" > "$W/$1.cc.txt" 2>&1 \
        || { reading m5.compile "$1 failed: $(one_line "$W/$1.cc.txt")"; return 1; }
    cat > "$app/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>probe</string>
<key>CFBundleIdentifier</key><string>$id</string>
<key>CFBundleName</key><string>probe</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneSimulator</string></array>
<key>MinimumOSVersion</key><string>17.0</string>
<key>UIDeviceFamily</key><array><integer>1</integer></array>
<key>LSRequiresIPhoneOS</key><true/>
</dict></plist>
P
    codesign --force --sign - "$app" > /dev/null 2>&1
}
make_app org.m634.fast 0 || exit 0
make_app org.m634.slow 1 || exit 0

UDID=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
d = json.load(sys.stdin)["devices"]
c = [x for rt, xs in d.items() if "iOS" in rt for x in xs
     if x.get("isAvailable") and x["name"].startswith("iPhone")]
print(c[-1]["udid"] if c else "")')
reading m5.device "$UDID $(xcrun simctl list devices | grep -m1 "$UDID" | tr -s ' ')"
[ -n "$UDID" ] || exit 0
xcrun simctl boot "$UDID" > /dev/null 2>&1
with_timeout 300 xcrun simctl bootstatus "$UDID" -b > boot.txt 2>&1
reading m5.boot "exit=$? $(tail -1 boot.txt)"
echo "M5_UDID=$UDID" >> "${GITHUB_ENV:-/dev/null}"
for id in org.m634.fast org.m634.slow; do
    xcrun simctl install "$UDID" "$W/$id.app" > "install-$id.txt" 2>&1
    reading m5.install "$id exit=$? $(one_line "install-$id.txt")"
done

measure() {  # measure <bundle-id> <spelling>
    local id="$1" sp="$2" i=0 seen=0 codes="" out rc f g
    while [ "$i" -lt "$N" ]; do
        i=$((i + 1))
        case "$sp" in
            pty)
                out=$(with_timeout 60 xcrun simctl launch --console-pty --terminate-running-process "$UDID" "$id" 2>&1); rc=$? ;;
            console)
                out=$(with_timeout 60 xcrun simctl launch --console --terminate-running-process "$UDID" "$id" 2>&1); rc=$? ;;
            files)
                f="$W/$id.$i.out"; g="$W/$id.$i.err"; : > "$f"; : > "$g"
                out=$(with_timeout 60 xcrun simctl launch --terminate-running-process --stdout="$f" --stderr="$g" "$UDID" "$id" 2>&1); rc=$?
                # Wait for the application to leave launchctl's list.
                local t=0
                while [ "$t" -lt 40 ]; do
                    if ! xcrun simctl spawn "$UDID" launchctl list 2>/dev/null | grep -F "$id" > "$W/ll.txt"; then break; fi
                    t=$((t + 1)); sleep 0.25
                done
                out="$out $(cat "$f" 2>/dev/null)" ;;
        esac
        case "$out" in *M634-MARKER*) seen=$((seen + 1)) ;; esac
        codes="$codes $rc"
    done
    reading "m5.$sp" "$id: marker in $seen/$N; exit codes:$(echo "$codes" | tr ' ' '\n' | sort | uniq -c | tr -s ' \n' ' ')"
}
for id in org.m634.fast org.m634.slow; do
    for sp in pty console files; do
        measure "$id" "$sp"
    done
done

# What launchctl shows for an application that has exited, if anything.
xcrun simctl spawn "$UDID" launchctl list 2>/dev/null | grep -i m634 > ll-final.txt
reading m5.launchctl-after "$(one_line ll-final.txt)"
exit 0
