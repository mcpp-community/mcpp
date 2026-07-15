#!/usr/bin/env bash
# requires: windows
# 97_mingw_toolchain.sh — Windows-native MinGW-w64 GCC via the xlings
# ecosystem (xim:mingw-gcc, winlibs GCC 16.1.0 UCRT):
#   - `toolchain install mingw 16.1.0` resolves + installs the xpkg
#   - `toolchain default mingw@16.1.0` persists; `list` stars `mingw 16.1.0`
#   - new → build → run works with import std + a named module (gcm pipeline)
#   - the produced exe is portable: runs from a clean dir without the
#     toolchain's bin on PATH (static libstdc++/libgcc defaults)
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

echo "PASS: mingw toolchain — install, default, modules build/run, standalone exe"
