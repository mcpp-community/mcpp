#!/usr/bin/env bash
# M5, run 3. Run 2 measured, over 120 launches, that every `simctl launch`
# spelling returns 0 for an application that exits 7, and that output was
# lost once (`--console`, the slow application). What remains:
#   1. the plugins CI's condition: a launch right after a fresh install;
#   2. whether `simctl spawn` of the INSTALLED application's executable
#      returns the application's own status and output;
#   3. what a launch returns for an application that crashes.
set +e
set -u
. "$(dirname "$0")/common.sh"

N=${M5_RUNS:-20}
W="$RUNNER_TEMP/m5s"
rm -rf "$W"; mkdir -p "$W"
cd "$W" || exit 1
SDK=$(xcrun --sdk iphonesimulator --show-sdk-path 2>/dev/null)
if [ -z "$SDK" ]; then reading m5s.skip "no iphonesimulator SDK"; exit 0; fi
with_timeout() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }

make_app() {  # make_app <bundle-id> <body>
    local id="$1" app="$W/$1.app"
    mkdir -p "$app"
    printf '#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\nint main(void) { %s }\n' "$2" > "$W/$1.c"
    xcrun --sdk iphonesimulator clang -target arm64-apple-ios17.0-simulator \
        -isysroot "$SDK" "$W/$1.c" -o "$app/probe" > "$W/$1.cc.txt" 2>&1 \
        || { reading m5s.compile "$1: $(one_line "$W/$1.cc.txt")"; return 1; }
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
make_app org.m634.exit7 'puts("M634-MARKER"); fflush(stdout); return 7;' || exit 0
make_app org.m634.crash 'puts("M634-MARKER"); fflush(stdout); abort();' || exit 0

UDID=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
d = json.load(sys.stdin)["devices"]
c = [x for rt, xs in d.items() if "iOS" in rt for x in xs
     if x.get("isAvailable") and x["name"].startswith("iPhone")]
print(c[-1]["udid"] if c else "")')
[ -n "$UDID" ] || { reading m5s.device "none"; exit 0; }
xcrun simctl boot "$UDID" > /dev/null 2>&1
with_timeout 300 xcrun simctl bootstatus "$UDID" -b > boot.txt 2>&1
reading m5s.boot "exit=$? device=$UDID"

# 1. A launch right after a fresh install, every time.
seen=0; codes=""
i=0
while [ "$i" -lt "$N" ]; do
    i=$((i + 1))
    xcrun simctl uninstall "$UDID" org.m634.exit7 > /dev/null 2>&1
    xcrun simctl install "$UDID" "$W/org.m634.exit7.app" > /dev/null 2>&1
    out=$(with_timeout 60 xcrun simctl launch --console-pty --terminate-running-process "$UDID" org.m634.exit7 2>&1); rc=$?
    case "$out" in *M634-MARKER*) seen=$((seen + 1)) ;; esac
    codes="$codes $rc"
done
reading m5s.pty-after-fresh-install "marker in $seen/$N; exit codes:$(echo "$codes" | tr ' ' '\n' | grep -v '^$' | sort | uniq -c | tr -s ' \n' ' ')"

# 2. `simctl spawn` of the installed application's executable.
APPDIR=$(xcrun simctl get_app_container "$UDID" org.m634.exit7 app 2>/dev/null)
reading m5s.app-container "${APPDIR:-none}"
seen=0; codes=""
i=0
while [ "$i" -lt "$N" ]; do
    i=$((i + 1))
    out=$(with_timeout 60 xcrun simctl spawn "$UDID" "$APPDIR/probe" 2>&1); rc=$?
    case "$out" in *M634-MARKER*) seen=$((seen + 1)) ;; esac
    codes="$codes $rc"
done
reading m5s.spawn-installed-executable "marker in $seen/$N; exit codes:$(echo "$codes" | tr ' ' '\n' | grep -v '^$' | sort | uniq -c | tr -s ' \n' ' ')"

# 3. A crashing application, through launch and through spawn.
xcrun simctl install "$UDID" "$W/org.m634.crash.app" > /dev/null 2>&1
out=$(with_timeout 60 xcrun simctl launch --console-pty --terminate-running-process "$UDID" org.m634.crash 2>&1); rc=$?
reading m5s.launch-crash "exit=$rc marker=$(case "$out" in *M634-MARKER*) echo yes ;; *) echo no ;; esac) out=$(printf '%s' "$out" | tr '\n' '|' | cut -c1-200)"
CDIR=$(xcrun simctl get_app_container "$UDID" org.m634.crash app 2>/dev/null)
out=$(with_timeout 60 xcrun simctl spawn "$UDID" "$CDIR/probe" 2>&1); rc=$?
reading m5s.spawn-crash "exit=$rc out=$(printf '%s' "$out" | tr '\n' '|' | cut -c1-200)"
exit 0
