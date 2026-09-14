#!/usr/bin/env bash
# M1: does a Mach-O program with a graph-built dylib run from a tree that
#     stages the dylib beside the program, with no load-command edit?
# B1: does a bundle with the dylib in Contents/Frameworks load it through an
#     rpath added at link time by a host module's `mcpp::link_flag`, and does
#     an ad-hoc signed bundle verify?
# B2: can a DMG be made, verified, mounted and detached from such a bundle
#     with the base system's tools?
set +e
set -u
. "$(dirname "$0")/common.sh"

W="$RUNNER_TEMP/m1"
rm -rf "$W"; mkdir -p "$W"
cd "$W" || exit 1

mkdir -p fw/src rules/src app/src app2/src
cat > fw/mcpp.toml <<'T'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
T
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > fw/src/fw.cppm

cat > rules/mcpp.toml <<'T'
[package]
namespace = "demo"
name      = "rules"
version   = "0.1.0"
[targets.rules]
kind = "lib"
T
cat > rules/src/rules.cppm <<'T'
export module demo.rules;
import std;
import mcpp;
export namespace demo::rules {
inline void bundle_rpath() {
    mcpp::link_flag("-Wl,-rpath,@executable_path/../Frameworks");
}
}
T

# app: the dependency is shared, and a host module adds the bundle rpath.
cat > app/mcpp.toml <<'T'
[package]
name    = "app"
version = "0.1.0"
[dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
[build-dependencies]
demo.rules = { path = "../rules", host-module = true }
[targets.app]
kind = "bin"
main = "src/main.cpp"
T
cat > app/build.mcpp <<'T'
import std;
import mcpp;
import demo.rules;
int main() { demo::rules::bundle_rpath(); return 0; }
T
MAIN='#include <cstdio>
import fw;
int main() { std::puts("1-2-3"); return fw_anchor() == 41 ? 7 : 1; }'
printf '%s\n' "$MAIN" > app/src/main.cpp

# app2: the control. The same program with no host module, so no bundle rpath.
cat > app2/mcpp.toml <<'T'
[package]
name    = "app2"
version = "0.1.0"
[dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
[targets.app2]
kind = "bin"
main = "src/main.cpp"
T
printf '%s\n' "$MAIN" > app2/src/main.cpp

(cd app && "$MCPP" build > build.log 2>&1); rc=$?
reading m1.build "app exit=$rc"
[ "$rc" -eq 0 ] || { tail -30 app/build.log; }
(cd app2 && "$MCPP" build > build.log 2>&1); rc2=$?
reading m1.build "app2 exit=$rc2"
[ "$rc2" -eq 0 ] || { tail -30 app2/build.log; }

BIN=$(find "$W/app/target" -type f -name app -perm -u+x 2>/dev/null | head -1)
DYLIB=$(find "$W/app/target" -name 'libfw.dylib' 2>/dev/null | head -1)
BIN2=$(find "$W/app2/target" -type f -name app2 -perm -u+x 2>/dev/null | head -1)
DYLIB2=$(find "$W/app2/target" -name 'libfw.dylib' 2>/dev/null | head -1)
reading m1.artifacts "bin=${BIN:-none} dylib=${DYLIB:-none}"
if [ -z "$BIN" ] || [ -z "$DYLIB" ]; then
    reading m1.abort "no artifacts; the remaining readings are not taken"
    exit 0
fi

otool -L "$BIN" > otool-L.txt 2>&1
reading m1.otool-L "$(grep libfw otool-L.txt | tr -s ' \t' ' ')"
otool -l "$BIN" > otool-l.txt 2>&1
reading m1.rpaths "app: $(awk '/LC_RPATH/{getline; getline; print $2}' otool-l.txt | tr '\n' ' ')"
otool -l "$BIN2" > otool-l2.txt 2>&1
reading m1.rpaths "app2 (control): $(awk '/LC_RPATH/{getline; getline; print $2}' otool-l2.txt | tr '\n' ' ')"
reading m1.install-name "$(otool -D "$DYLIB" | tail -1)"
"$BIN" > run.out 2>&1; reading m1.run-build-tree "exit=$? out=$(one_line run.out)"

# The staged layout this record proposes: the dylib beside the program.
mkdir -p stage/bin
cp "$BIN" "$DYLIB" stage/bin/
mv app/target app/target.moved
stage/bin/app > stage.out 2>&1; reading m1.staged-beside "exit=$? out=$(one_line stage.out)"
rm stage/bin/libfw.dylib
stage/bin/app > stage-neg.out 2>&1; reading m1.staged-beside-negative "exit=$? out=$(one_line stage-neg.out)"
mv app/target.moved app/target

# Today's engine on this row, for the record.
(cd app && "$MCPP" pack --format dir > pack.log 2>&1); rc=$?
reading m1.pack-dir-today "exit=$rc $(grep -m1 -i 'cannot package' app/pack.log)"

# ── B1: the bundle ──────────────────────────────────────────────────────────
make_bundle() {  # make_bundle <dir.app> <exe> <dylib> <exe-name>
    local A="$1/Contents"
    rm -rf "$1"; mkdir -p "$A/MacOS" "$A/Frameworks"
    cp "$2" "$A/MacOS/$4"
    cp "$3" "$A/Frameworks/"
    cat > "$A/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$4</string>
<key>CFBundleIdentifier</key><string>org.m634.$4</string>
<key>CFBundleName</key><string>$4</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
</dict></plist>
P
}
make_bundle "$W/Demo.app" "$BIN" "$DYLIB" app
codesign --verify --deep --strict --verbose=2 "$W/Demo.app" > verify-unsigned.txt 2>&1
reading b1.verify-before-signing "exit=$? $(one_line verify-unsigned.txt)"
"$W/Demo.app/Contents/MacOS/app" > b1run.out 2>&1
reading b1.run-bundled "exit=$? out=$(one_line b1run.out)"
codesign --force --sign - "$W/Demo.app/Contents/Frameworks/libfw.dylib" > sign1.txt 2>&1
codesign --force --sign - "$W/Demo.app" > sign2.txt 2>&1
reading b1.sign-adhoc "dylib: $(one_line sign1.txt) bundle: $(one_line sign2.txt)"
codesign --verify --deep --strict --verbose=2 "$W/Demo.app" > verify-signed.txt 2>&1
reading b1.verify-after-signing "exit=$? $(one_line verify-signed.txt)"
"$W/Demo.app/Contents/MacOS/app" > b1run2.out 2>&1
reading b1.run-signed "exit=$? out=$(one_line b1run2.out)"
mv "$W/Demo.app/Contents/Frameworks/libfw.dylib" "$W/libfw.dylib.away"
"$W/Demo.app/Contents/MacOS/app" > b1neg.out 2>&1
reading b1.run-without-framework "exit=$? out=$(one_line b1neg.out)"
mv "$W/libfw.dylib.away" "$W/Demo.app/Contents/Frameworks/libfw.dylib"

if [ -n "$BIN2" ] && [ -n "$DYLIB2" ]; then
    make_bundle "$W/Control.app" "$BIN2" "$DYLIB2" app2
    "$W/Control.app/Contents/MacOS/app2" > ctl.out 2>&1
    reading b1.control-no-link-flag "exit=$? out=$(one_line ctl.out)"
fi

# ── B1, run 3: the layout itself, with the rpath the link would carry ──────
# Run 2 measured that the engine anchors `-Wl,-rpath,@executable_path/...` to
# the package directory, so the link-time route cannot be measured before that
# is fixed. This adds the literal rpath after link instead, which is the state
# a fixed link produces, and re-signs because the file changed.
make_bundle "$W/Fixed.app" "$BIN2" "$DYLIB2" app2
install_name_tool -add_rpath @executable_path/../Frameworks "$W/Fixed.app/Contents/MacOS/app2" > int.txt 2>&1
reading b1.post-link-rpath "exit=$? rpaths=$(otool -l "$W/Fixed.app/Contents/MacOS/app2" | awk '/LC_RPATH/{getline; getline; print $2}' | tr '\n' ' ')"
"$W/Fixed.app/Contents/MacOS/app2" > fixed-unsigned.out 2>&1
reading b1.post-link-run-before-resign "exit=$? out=$(one_line fixed-unsigned.out)"
codesign --force --sign - "$W/Fixed.app/Contents/Frameworks/libfw.dylib" > /dev/null 2>&1
codesign --force --sign - "$W/Fixed.app/Contents/MacOS/app2" > /dev/null 2>&1
codesign --force --sign - "$W/Fixed.app" > /dev/null 2>&1
codesign --verify --deep --strict --verbose=2 "$W/Fixed.app" > fixed-verify.txt 2>&1
reading b1.post-link-verify "exit=$? $(one_line fixed-verify.txt)"
"$W/Fixed.app/Contents/MacOS/app2" > fixed.out 2>&1
reading b1.post-link-run "exit=$? out=$(one_line fixed.out)"
mv "$W/Fixed.app/Contents/Frameworks/libfw.dylib" "$W/libfw.fixed.away"
"$W/Fixed.app/Contents/MacOS/app2" > fixed-neg.out 2>&1
reading b1.post-link-run-without-framework "exit=$? out=$(one_line fixed-neg.out | cut -c1-160)"
mv "$W/libfw.fixed.away" "$W/Fixed.app/Contents/Frameworks/libfw.dylib"

# ── B2: the DMG ─────────────────────────────────────────────────────────────
mkdir -p dmgstage
cp -R "$W/Demo.app" dmgstage/
ln -s /Applications dmgstage/Applications
hdiutil create -volname Demo -srcfolder dmgstage -format UDZO -ov Demo.dmg > dmg-create.txt 2>&1
reading b2.create "exit=$? $(tail -1 dmg-create.txt)"
hdiutil verify Demo.dmg > dmg-verify.txt 2>&1
reading b2.verify "exit=$? $(tail -1 dmg-verify.txt)"
mkdir -p mnt
hdiutil attach -nobrowse -readonly -mountpoint "$W/mnt" Demo.dmg > dmg-attach.txt 2>&1
reading b2.attach "exit=$?"
[ -x "$W/mnt/Demo.app/Contents/MacOS/app" ] && e=yes || e=no
[ -L "$W/mnt/Applications" ] && l="$(readlink "$W/mnt/Applications")" || l=no
reading b2.layout "executable=$e applications-link=$l"
hdiutil detach "$W/mnt" > dmg-detach.txt 2>&1
reading b2.detach "exit=$?"
exit 0
