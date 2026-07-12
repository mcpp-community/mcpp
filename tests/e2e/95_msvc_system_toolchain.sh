#!/usr/bin/env bash
# requires: msvc
# 95_msvc_system_toolchain.sh — msvc@system detection & selection (Windows):
#   - `toolchain default msvc` locates + identifies the system MSVC and
#     persists the stable spec msvc@system
#   - `toolchain list` shows the detected MSVC in a System section, starred
#   - version pin-verify: `msvc@99` mismatches the detected install
#   - `toolchain remove/install msvc`: mcpp never manages MSVC itself
#   - `mcpp build` fails with the owned "not yet supported" gate message
set -e

# This test flips the global default toolchain; save + restore it so later
# tests / CI steps keep their configured toolchain.
CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
    ORIG_DEFAULT=$(sed -n '/^\[toolchain\]/,/^\[/p' "$CONF" \
        | grep '^default' | head -1 | cut -d'"' -f2 || true)
fi
TMP=$(mktemp -d)
restore() {
    if [[ -n "$ORIG_DEFAULT" ]]; then
        "$MCPP" toolchain default "$ORIG_DEFAULT" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap restore EXIT

# Neutral cwd: `toolchain list` stars the *effective* default, and a project
# mcpp.toml [toolchain] in the cwd (e.g. the mcpp repo root, where run_all.sh
# executes) would shadow the global default we're about to set.
cd "$TMP"

# 1) switch to msvc: detect + identify + persist
out=$("$MCPP" toolchain default msvc 2>&1) || { echo "FAIL: default msvc: $out"; exit 1; }
[[ "$out" == *"Detected"* ]]     || { echo "FAIL: no Detected line: $out"; exit 1; }
[[ "$out" == *"msvc "* ]]        || { echo "FAIL: no msvc version: $out"; exit 1; }
[[ "$out" == *"msvc@system"* ]]  || { echo "FAIL: default not msvc@system: $out"; exit 1; }
[[ "$out" == *"cl:"* ]]          || { echo "FAIL: no cl path: $out"; exit 1; }

# 2) list shows the System section with the effective-default star
out=$("$MCPP" toolchain list 2>&1)
[[ "$out" == *"System:"* ]] || { echo "FAIL: no System section: $out"; exit 1; }
echo "$out" | grep -E '\*\s*msvc' >/dev/null \
    || { echo "FAIL: msvc row not starred as default: $out"; exit 1; }

# 3) version pin-verify: an impossible major must mismatch
rc=0; out=$("$MCPP" toolchain default msvc@99 2>&1) || rc=$?
[[ $rc -ne 0 ]]                 || { echo "FAIL: msvc@99 should mismatch"; exit 1; }
[[ "$out" == *"requested"* ]]   || { echo "FAIL: mismatch message: $out"; exit 1; }

# 4) mcpp never manages the MSVC installation
rc=0; out=$("$MCPP" toolchain remove msvc 2>&1) || rc=$?
[[ $rc -ne 0 && "$out" == *"system toolchain"* ]] \
    || { echo "FAIL: remove msvc: rc=$rc out=$out"; exit 1; }
out=$("$MCPP" toolchain install msvc 2>&1) \
    || { echo "FAIL: install msvc (present) should exit 0: $out"; exit 1; }
[[ "$out" == *"already installed"* ]] \
    || { echo "FAIL: install msvc message: $out"; exit 1; }

# 5) native cl.exe builds are gated with one owned message
"$MCPP" new hello_msvc >/dev/null 2>&1
cd hello_msvc
rc=0; out=$("$MCPP" build 2>&1) || rc=$?
[[ $rc -ne 0 ]]                        || { echo "FAIL: build should be gated"; exit 1; }
[[ "$out" == *"not yet supported"* ]]  || { echo "FAIL: gate message: $out"; exit 1; }
[[ "$out" == *"llvm@20.1.7"* ]]        || { echo "FAIL: no alternative hint: $out"; exit 1; }

echo "PASS: msvc@system detection, selection, guidance, and build gate"
