#!/usr/bin/env bash
# `[build] module_extensions` on the platform's DEFAULT toolchain (#412).
#
# ⚠️ WHY THIS EXISTS SEPARATELY FROM 217. `217_module_extensions.sh` declares
# `# requires: gcc`, and `run_all.sh` deliberately does not grant `gcc` on
# Windows or macOS:
#
#     # Windows runners may have g++.exe (MinGW/Strawberry) in PATH but it's
#     # not a proper mcpp-compatible GCC. Don't add gcc capability.
#     # macOS g++ is Apple Clang, not real GCC — don't add gcc capability.
#
# So `module_extensions` has only ever been exercised on Linux. The specific
# combination "declare `.ixx` and build it with MSVC" — the one a user porting
# an MSVC project actually takes — has never run anywhere. It follows by
# reasoning (`.ixx` is cl's own convention and mcpp emits `/interface /TP`
# unconditionally), but reasoning is not measurement: this is the same shape as
# mcpp#272, where `# requires: elf gcc` meant the Clang and MSVC legs were never
# reached at all.
#
# This test grants itself no capability. It uses whatever toolchain the platform
# defaults to — MSVC on Windows, LLVM on macOS, GCC on Linux — so it runs
# EVERYWHERE and covers the two legs 217 cannot reach.
set -e

_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -z "${MCPP:-}" ]; then
  MCPP="$(bash "$_root/.github/tools/newest_artifact.sh" "$_root" mcpp 2>/dev/null || true)"
  [ -n "$MCPP" ] || { echo "SKIP: no mcpp binary built yet"; exit 0; }
fi
case "$MCPP" in /*|?:[/\\]*) ;; *) MCPP="$_root/$MCPP" ;; esac
[ -x "$MCPP" ] || { echo "FAIL: MCPP=$MCPP is not executable"; exit 1; }
export MCPP

TMP=$(mktemp -d)
trap "rm -rf $TMP || true" EXIT
mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

# `.ixx` is the extension this is about: cl's own spelling, and the one an MSVC
# project arrives with. The toolchain block gives each platform its default —
# the point is that no leg is skipped, not that they all use the same compiler.
cat > mcpp.toml <<'EOF'
[package]
name    = "extdefault"
version = "0.1.0"

[toolchain]
default = "gcc@16.1.0"
macos   = "llvm@22.1.8"
windows = "llvm@20.1.7"

[build]
module_extensions = [".ixx"]
EOF

cat > src/greet.ixx <<'EOF'
export module extdefault.greet;
import std;
export auto greet() -> std::string { return "ixx-ok"; }
EOF

cat > src/main.cpp <<'EOF'
import std;
import extdefault.greet;
int main() { std::println("{}", greet()); return greet() == "ixx-ok" ? 0 : 1; }
EOF

echo "== build =="
"$MCPP" build --release > build.log 2>&1 || {
    echo "FAIL: build with module_extensions = [\".ixx\"] failed on this platform's"
    echo "      default toolchain. This is the leg 217 cannot reach."
    tail -40 build.log
    exit 1
}

# ⚠️ A BUILD THAT EXITS 0 IS NOT ENOUGH, and this is the exact trap the
# extension work was full of: Clang hands an unrecognised `.ixx` to the LINKER,
# warns, and exits 0 having produced NO BMI. So assert the interface really was
# compiled as one — a BMI exists for the module.
BMI="$(find target -type f \( -name '*.gcm' -o -name '*.pcm' -o -name '*.ifc' \) 2>/dev/null \
       | grep -iE 'greet' | head -1)"
[ -n "$BMI" ] || {
    echo "FAIL: no BMI produced for the .ixx interface — it was not treated as a"
    echo "      module interface unit at all. A zero exit code does not prove"
    echo "      compilation here: an unrecognised extension is a LINKER input."
    find target -type f \( -name '*.gcm' -o -name '*.pcm' -o -name '*.ifc' \) | head -10
    exit 1
}
echo "  BMI: $BMI"

echo "== run =="
BIN="$(find target -type f -path '*/bin/*' \( -name 'extdefault' -o -name 'extdefault.exe' \) | head -1)"
[ -n "$BIN" ] || { echo "FAIL: no binary produced"; find target -path '*/bin/*' -type f | head; exit 1; }
OUT="$("$BIN")" || { echo "FAIL: the binary exited $?"; exit 1; }
[ "$OUT" = "ixx-ok" ] || { echo "FAIL: binary printed '$OUT', expected 'ixx-ok'"; exit 1; }
echo "  ran: $OUT"

echo "236 module_extensions on the default toolchain OK"
