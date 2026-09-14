#!/usr/bin/env bash
# M6: do the WiX native SDK archives `xim:wix` 5.0.2 ships (MSVC `v14`
# objects) link under mcpp's Windows toolchains: the default (clang with the
# MSVC ABI when MSVC is present) and `msvc@system`?
set +e
set -u
. "$(dirname "$0")/common.sh"

D="$RUNNER_TEMP/m6/wixlink"
rm -rf "$D"; mkdir -p "$D/src"
cat > "$D/mcpp.toml" <<'T'
[package]
name    = "wixlink"
version = "0.1.0"

[xlings.workspace]
"xim:wix" = "5.0.2"

[targets.wixlink]
kind = "bin"
main = "src/main.c"
T
cat > "$D/build.mcpp" <<'T'
import std;
import mcpp;
int main() {
    std::string w = mcpp::xpkg_dir("xim", "wix");
    if (w.empty()) { mcpp::warning("xim:wix answered no directory"); return 1; }
    mcpp::include_dir((w + "/dutil/build/native/include").c_str());
    mcpp::link_search((w + "/dutil/build/native/v14/x64").c_str());
    mcpp::link_search((w + "/bootstrapper/build/native/v14/x64").c_str());
    mcpp::link_lib("dutil");
    mcpp::link_lib("balutil");
    // The first run named no system import library, and every unresolved
    // symbol was one of these (MessageBoxA, RegOpenKeyExW, CoInitializeEx):
    // the archives' own symbols all resolved on both MSVC-ABI toolchains.
    for (auto lib : {"user32", "advapi32", "ole32", "oleaut32", "shell32",
                     "shlwapi", "version", "crypt32", "wininet", "msi",
                     "rpcrt4", "uuid", "gdi32", "comctl32", "wintrust"})
        mcpp::link_lib(lib);
    return 0;
}
T
cat > "$D/src/main.c" <<'C'
#include <windows.h>
extern HRESULT __stdcall DutilInitialize(void *callback);
extern void __stdcall DutilUninitialize(void);
extern void __stdcall BalUninitialize(void);
int main(void) {
    HRESULT hr = DutilInitialize(NULL);
    DutilUninitialize();
    void (*keep)(void) = BalUninitialize;   /* pulls balutil's object in */
    (void)keep;
    return SUCCEEDED(hr) ? 0 : 3;
}
C
cd "$D" || exit 1

run_leg() {  # run_leg <label> [mcpp flags...]
    local label="$1"; shift
    rm -rf target
    "$MCPP" build "$@" > "build-$label.log" 2>&1; local rc=$?
    reading "m6.$label.build" "exit=$rc $(grep -m1 -E 'Resolved ' "build-$label.log" | tr -s ' ')"
    if [ "$rc" -ne 0 ]; then
        reading "m6.$label.errors" "$(grep -iE 'error|undefined|unresolved' "build-$label.log" | head -8 | tr '\n' '|' | cut -c1-900)"
        return
    fi
    local exe
    exe=$(find target -name 'wixlink.exe' | head -1)
    "./$exe" > "run-$label.out" 2>&1
    reading "m6.$label.run" "exit=$? exe=$exe"
}
run_leg default
run_leg msvc --toolchain msvc@system
exit 0
