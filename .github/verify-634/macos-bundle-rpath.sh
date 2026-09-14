#!/usr/bin/env bash
# B1, run 4. Run 3's `install_name_tool -add_rpath` on an mcpp-built program
# exited 1 and left the rpaths unchanged. This run records why, and measures
# the two routes that remain:
#   A. the rpath carried by the link itself (what the engine produces once it
#      stops anchoring `@executable_path`): a program linked directly with
#      `-rpath @executable_path/../Frameworks`, bundled, signed, run;
#   B. an mcpp-built program linked with `-headerpad_max_install_names`
#      (through `mcpp::link_flag`, which does not anchor that flag), then
#      edited with `install_name_tool -add_rpath`, re-signed, run.
set +e
set -u
. "$(dirname "$0")/common.sh"

W="$RUNNER_TEMP/b1r"
rm -rf "$W"; mkdir -p "$W"
cd "$W" || exit 1

bundle() {  # bundle <dir.app> <exe> <dylib> <exe-name>
    local A="$1/Contents"
    rm -rf "$1"; mkdir -p "$A/MacOS" "$A/Frameworks"
    cp "$2" "$A/MacOS/$4"; cp "$3" "$A/Frameworks/"
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>%s</string><key>CFBundleIdentifier</key><string>org.m634.%s</string><key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n' "$4" "$4" > "$A/Info.plist"
}
sign_run() {  # sign_run <label> <dir.app> <exe-name>
    codesign --force --sign - "$2/Contents/Frameworks/"*.dylib > /dev/null 2>&1
    codesign --force --sign - "$2" > /dev/null 2>&1
    codesign --verify --deep --strict "$2" > "$1.verify" 2>&1
    reading "b1r.$1.verify" "exit=$? $(one_line "$1.verify")"
    "$2/Contents/MacOS/$3" > "$1.out" 2>&1
    reading "b1r.$1.run" "exit=$? out=$(one_line "$1.out" | cut -c1-200)"
    mv "$2/Contents/Frameworks" "$2/Frameworks.away"
    "$2/Contents/MacOS/$3" > "$1.neg" 2>&1
    reading "b1r.$1.run-without-framework" "exit=$? out=$(one_line "$1.neg" | cut -c1-120)"
    mv "$2/Frameworks.away" "$2/Contents/Frameworks"
}

# ── A: the rpath carried by the link ───────────────────────────────────────
printf 'int fw_anchor(void) { return 41; }\n' > fw.c
printf '#include <stdio.h>\nint fw_anchor(void);\nint main(void) { puts("1-2-3"); return fw_anchor() == 41 ? 7 : 1; }\n' > app.c
xcrun clang -dynamiclib fw.c -install_name @rpath/libfw.dylib -o libfw.dylib > a-cc.txt 2>&1
xcrun clang app.c -L. -lfw -Wl,-rpath,@executable_path/../Frameworks -o linkapp >> a-cc.txt 2>&1
reading b1r.A.link "exit=$? rpaths=$(otool -l linkapp | awk '/LC_RPATH/{getline; getline; print $2}' | tr '\n' ' ') $(one_line a-cc.txt)"
bundle "$W/A.app" linkapp libfw.dylib linkapp
sign_run A "$W/A.app" linkapp

# ── B: an mcpp-built program, headerpad, then an rpath edit ────────────────
mkdir -p fwpkg/src rules/src app/src
printf '[package]\nnamespace = "demo"\nname = "fw"\nversion = "0.1.0"\n[targets.fw]\nkind = "lib"\n' > fwpkg/mcpp.toml
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > fwpkg/src/fw.cppm
printf '[package]\nnamespace = "demo"\nname = "rules"\nversion = "0.1.0"\n[targets.rules]\nkind = "lib"\n' > rules/mcpp.toml
printf 'export module demo.rules;\nimport std;\nimport mcpp;\nexport namespace demo::rules {\ninline void pad() { mcpp::link_flag("-Wl,-headerpad_max_install_names"); }\n}\n' > rules/src/rules.cppm
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../fwpkg", linkage = "shared" }\n[build-dependencies]\ndemo.rules = { path = "../rules", host-module = true }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > app/mcpp.toml
printf 'import std;\nimport mcpp;\nimport demo.rules;\nint main() { demo::rules::pad(); return 0; }\n' > app/build.mcpp
printf '#include <cstdio>\nimport fw;\nint main() { std::puts("1-2-3"); return fw_anchor() == 41 ? 7 : 1; }\n' > app/src/main.cpp
(cd app && "$MCPP" build > build.log 2>&1); rc=$?
reading b1r.B.build "exit=$rc"
[ "$rc" -eq 0 ] || tail -30 app/build.log
BIN=$(find "$W/app/target" -type f -name app -perm -u+x | head -1)
DYL=$(find "$W/app/target" -name libfw.dylib | head -1)

# First, an unpadded mcpp build (no host module), with the tool's message.
mkdir -p app0/src
printf '[package]\nname = "app0"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../fwpkg", linkage = "shared" }\n[targets.app0]\nkind = "bin"\nmain = "src/main.cpp"\n' > app0/mcpp.toml
cp app/src/main.cpp app0/src/main.cpp
(cd app0 && "$MCPP" build > build.log 2>&1)
BIN0=$(find "$W/app0/target" -type f -name app0 -perm -u+x | head -1)
cp "$BIN0" nopad-probe
install_name_tool -add_rpath @executable_path/../Frameworks nopad-probe > nopad.txt 2>&1
reading b1r.B.edit-without-headerpad "exit=$? $(one_line nopad.txt)"

bundle "$W/B.app" "$BIN" "$DYL" app
install_name_tool -add_rpath @executable_path/../Frameworks "$W/B.app/Contents/MacOS/app" > b-int.txt 2>&1
reading b1r.B.edit "exit=$? rpaths=$(otool -l "$W/B.app/Contents/MacOS/app" | awk '/LC_RPATH/{getline; getline; print $2}' | tr '\n' ' ') $(one_line b-int.txt)"
codesign --force --sign - "$W/B.app/Contents/MacOS/app" > /dev/null 2>&1
sign_run B "$W/B.app" app
exit 0
