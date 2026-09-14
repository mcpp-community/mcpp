#!/usr/bin/env bash
# M4: does the main bundle resolve when a bundle's executable is run
#     directly (the premise of a `macapp-run` that simply execs it)?
# B3: does a named runner reach a `--format app` bundle today, does a plain
#     `mcpp run` stay bare, and does a DEFAULT runner (`open -W`, the shape
#     HuxerUI's manifests use) also wrap a plain `mcpp run`?
set +e
set -u
. "$(dirname "$0")/common.sh"

W="$RUNNER_TEMP/m4"
rm -rf "$W"; mkdir -p "$W"
cd "$W" || exit 1

# ── M4: CoreFoundation's main bundle, in and out of a bundle ───────────────
cat > probe.c <<'C'
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
int main(void) {
    CFBundleRef b = CFBundleGetMainBundle();
    char path[4096] = "none";
    CFURLRef u = b ? CFBundleCopyBundleURL(b) : NULL;
    if (u) CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path);
    printf("bundle=%s\n", path);
    char res[4096] = "none";
    CFURLRef r = b ? CFBundleCopyResourceURL(b, CFSTR("greeting"), CFSTR("txt"), NULL) : NULL;
    if (r) CFURLGetFileSystemRepresentation(r, true, (UInt8 *)res, sizeof res);
    printf("resource=%s\n", res);
    puts("1-2-3");
    return 7;
}
C
xcrun clang -framework CoreFoundation probe.c -o probe > cc.txt 2>&1
reading m4.compile "exit=$? $(one_line cc.txt)"
mkdir -p N.app/Contents/MacOS N.app/Contents/Resources loose
cp probe N.app/Contents/MacOS/probe
echo "hello" > N.app/Contents/Resources/greeting.txt
cat > N.app/Contents/Info.plist <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>probe</string>
<key>CFBundleIdentifier</key><string>org.m634.nsbundle</string>
<key>CFBundlePackageType</key><string>APPL</string>
</dict></plist>
P
"$W/N.app/Contents/MacOS/probe" > in.out 2>&1
reading m4.exec-in-bundle "exit=$? $(one_line in.out)"
cp probe loose/probe
"$W/loose/probe" > loose.out 2>&1
reading m4.exec-loose-control "exit=$? $(one_line loose.out)"

# ── B3: a bundle through mcpp, dist-apple and a named runner ───────────────
mkdir -p rapp/src rapp/share
cat > rapp/mcpp.toml <<'T'
[package]
name    = "rapp"
version = "0.1.0"

[language]
standard   = "c++23"
modules    = true
import_std = true

[build-dependencies.mcpp]
plugins = { version = "0.9.3", features = ["dist-apple"], host-module = true }

[targets.rapp]
kind = "bin"
main = "src/main.cpp"

[target.aarch64-macos.runners]
app = ["/bin/sh", "-c", "exec \"$0/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \"$0/Contents/Info.plist\")\" \"$@\"", "{}"]
T
cat > rapp/build.mcpp <<'T'
import std;
import mcpp;
import mcpp.dist.apple;
int main() {
    mcpp::dist::apple::options opt;
    opt.target   = "rapp";
    opt.app_name = "RApp";
    return mcpp::dist::apple::generate(opt) ? 0 : 1;
}
T
printf '#include <cstdio>\nint main(int argc, char**) { std::printf("1-2-3 argc=%%d\\n", argc); return 7; }\n' > rapp/src/main.cpp

cd "$W/rapp" || exit 1
"$MCPP" build > build.log 2>&1; rc=$?
reading b3.build "exit=$rc $(tail -2 build.log | tr '\n' '|')"
[ "$rc" -eq 0 ] || tail -40 build.log
"$MCPP" run --format app --runner app -- extra > fmt-runner.out 2>&1
rc=$?; reading b3.run-format-app-runner-app "exit=$rc $(grep -E '1-2-3|Running|error' fmt-runner.out | tr '\n' '|')"; [ "$rc" -eq 7 ] || tail -25 fmt-runner.out
"$MCPP" run --format app > fmt-default.out 2>&1
reading b3.run-format-app-no-runner "exit=$? $(grep -E '1-2-3|Running|error' fmt-default.out | tr '\n' '|' | cut -c1-400)"
"$MCPP" run > plain.out 2>&1
reading b3.plain-run "exit=$? $(grep -E '1-2-3|Running|error' plain.out | tr '\n' '|')"

# The default-runner shape: `open -W`, with a stub `open` first on PATH that
# records what it was handed and exits 0.
mkdir -p "$W/stub"
cat > "$W/stub/open" <<'S'
#!/bin/sh
echo "open-stub argv: $*" >> "$OPEN_STUB_LOG"
exit 0
S
chmod +x "$W/stub/open"
cat >> mcpp.toml <<'T'

[target.aarch64-macos]
runner = ["open", "-W"]
T
export OPEN_STUB_LOG="$W/open-stub.log"
: > "$OPEN_STUB_LOG"
PATH="$W/stub:$PATH" "$MCPP" run > open-plain.out 2>&1
reading b3.default-runner-wraps-plain-run "exit=$? stub-log=$(one_line "$OPEN_STUB_LOG") out=$(grep -E 'Running|1-2-3' open-plain.out | tr '\n' '|')"
exit 0
