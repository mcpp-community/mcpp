#!/usr/bin/env bash
# requires: windows
# 97_mingw_toolchain.sh — Windows-native MinGW-w64 GCC via the xlings
# ecosystem (xim:mingw-gcc, winlibs GCC 16.1.0 UCRT):
#   - `toolchain install mingw 16.1.0` resolves + installs the xpkg
#   - `toolchain default mingw@16.1.0` persists; `list` stars `mingw 16.1.0`
#   - new → build → run works with import std + a named module (gcm pipeline)
#   - the produced exe is portable: runs from a clean dir without the
#     toolchain's bin on PATH (static libstdc++/libgcc defaults)
#   - so is the build.mcpp HOST helper mcpp execs mid-build (#299)
set -e

CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
    # NB: match `default =` exactly — `default_target =` also starts with
    # "default" (the persisted pair since the naming unification).
    ORIG_DEFAULT=$(sed -n '/^\[toolchain\]/,/^\[/p' "$CONF" \
        | grep -E '^default[[:space:]]*=' | head -1 | cut -d'"' -f2 || true)
fi
TMP=$(mktemp -d)
restore() {
    if [[ -n "$ORIG_DEFAULT" ]]; then
        "$MCPP" toolchain default "$ORIG_DEFAULT" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap restore EXIT

cd "$TMP"   # neutral cwd — project mcpp.toml [toolchain] must not shadow

# 1) install via the xlings ecosystem (xlings-res mirror)
out=$("$MCPP" toolchain install mingw 16.1.0 2>&1) \
    || { echo "FAIL: install mingw: $out"; exit 1; }
[[ "$out" == *"Installed"* || "$out" == *"already"* || "$out" == *"mingw"* ]] \
    || { echo "FAIL: install output: $out"; exit 1; }

# 2) switch default (legacy spelling — normalizes to the pair
#    gcc@16.1.0 + x86_64-windows-gnu) + list stars both axes
"$MCPP" toolchain default mingw@16.1.0 \
    || { echo "FAIL: default mingw@16.1.0"; exit 1; }
out=$("$MCPP" toolchain list 2>&1)
echo "$out" | grep -E '\*\s*gcc 16\.1\.0' >/dev/null \
    || { echo "FAIL: gcc 16.1.0 not starred in Toolchains: $out"; exit 1; }
echo "$out" | grep -E '\*\s*x86_64-windows-gnu' >/dev/null \
    || { echo "FAIL: x86_64-windows-gnu not starred in Targets: $out"; exit 1; }

# 3) real build: import std + named module through the gcm pipeline
"$MCPP" new hello_mingw >/dev/null 2>&1
cd hello_mingw
mkdir -p src
cat > src/greet.cppm <<'EOF'
export module hello.greet;
import std;
export namespace hello {
std::string greet() { return "mingw-ok"; }
}
EOF
cat > src/main.cpp <<'EOF'
import std;
import hello.greet;
int main() { std::println("{}", hello::greet()); return 0; }
EOF
out=$("$MCPP" build 2>&1) || { echo "FAIL: build: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run: $run_out"; exit 1; }
[[ "$run_out" == *"mingw-ok"* ]] || { echo "FAIL: run output: $run_out"; exit 1; }

# 4) portability: the exe's import table must not reference toolchain DLLs
#    (libstdc++-6 / libgcc_s / libwinpthread — static_stdlib defaults), and
#    it must run from a clean dir without the toolchain bin on PATH.
EXE=$(find target -name "hello_mingw.exe" -path "*/bin/*" | head -1)
[[ -n "$EXE" ]] || { echo "FAIL: no exe produced"; exit 1; }
OBJDUMP=$(ls "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-mingw-gcc/*/bin/objdump.exe 2>/dev/null | head -1)
imports=""
if [[ -x "$OBJDUMP" ]]; then
    imports=$("$OBJDUMP" -p "$EXE" 2>/dev/null | grep -i "DLL Name" || true)
    echo "exe imports:"; echo "$imports"
    bad=$(echo "$imports" | grep -iE "libstdc|libgcc|libwinpthread" || true)
    [[ -z "$bad" ]] || { echo "FAIL: toolchain DLLs in import table: $bad"; exit 1; }
fi
ISO="$TMP/iso"; mkdir -p "$ISO"
cp "$EXE" "$ISO/"
iso_rc=0
iso_out=$(cd "$ISO" && PATH="/usr/bin:/c/Windows/System32" ./hello_mingw.exe 2>&1) || iso_rc=$?
if [[ $iso_rc -ne 0 || "$iso_out" != *"mingw-ok"* ]]; then
    echo "FAIL: standalone run rc=$iso_rc out='$iso_out'"
    echo "$imports"
    exit 1
fi

# 5) the build.mcpp HOST helper is held to the same bar (#299). It is exec'd
#    by mcpp mid-build, and PE has no rpath — a dynamic helper resolves
#    libstdc++-6 / libgcc_s / libwinpthread through the process PATH and dies
#    with STATUS_DLL_NOT_FOUND (exit -1073741511) unless the user added the
#    toolchain bin to PATH by hand.
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    FILE* marker = std::fopen("helper-ran", "w");
    if (!marker) return 2;
    std::fputs("ok\n", marker);
    std::fclose(marker);
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF
out=$("$MCPP" build 2>&1) || { echo "FAIL: build with build.mcpp: $out"; exit 1; }
[[ -f helper-ran ]] || { echo "FAIL: build.mcpp did not run: $out"; exit 1; }
HELPER="target/.build-mcpp/build.mcpp.exe"
[[ -f "$HELPER" ]] || { echo "FAIL: no helper binary at $HELPER"; exit 1; }
if [[ -x "$OBJDUMP" ]]; then
    h_imports=$("$OBJDUMP" -p "$HELPER" 2>/dev/null | grep -i "DLL Name" || true)
    echo "helper imports:"; echo "$h_imports"
    bad=$(echo "$h_imports" | grep -iE "libstdc|libgcc|libwinpthread" || true)
    [[ -z "$bad" ]] || { echo "FAIL: toolchain DLLs in helper import table: $bad"; exit 1; }
fi
HISO="$TMP/hiso"; mkdir -p "$HISO"
cp "$HELPER" "$HISO/"
h_rc=0
(cd "$HISO" && PATH="/usr/bin:/c/Windows/System32" ./build.mcpp.exe >/dev/null 2>&1) || h_rc=$?
[[ $h_rc -eq 0 ]] || {
    echo "FAIL: build.mcpp helper does not run without the toolchain bin on PATH (rc=$h_rc)"
    exit 1
}

echo "PASS: mingw toolchain — install, default, modules build/run, standalone exe + build.mcpp helper"
