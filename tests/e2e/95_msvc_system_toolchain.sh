#!/usr/bin/env bash
# requires: msvc
# 95_msvc_system_toolchain.sh — msvc@system detection & selection (Windows):
#   - `toolchain default msvc` locates + identifies the system MSVC and
#     persists the stable spec msvc@system
#   - `toolchain list` shows the detected MSVC in a System section, starred
#   - a VERSIONED msvc spec is NOT this origin: it is a payload, and an
#     absent one says so instead of silently using the machine's compiler
#   - the retired `msvc@<cl-version>` spelling names both replacements
#   - `toolchain remove/install msvc`: mcpp never manages the machine's VS
set -e

# This test flips the global default toolchain; save + restore it so later
# tests / CI steps keep their configured toolchain.
CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
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

# 3) a VERSIONED spec is a PAYLOAD, not this origin.
#
#    This is the load-bearing assertion of the whole split: a toolset that is
#    not installed must FAIL. Falling back to the machine's compiler is
#    exactly the silent substitution the version axis exists to prevent, and
#    it would look like success from the outside.
rc=0; out=$("$MCPP" toolchain default msvc@14.0.99999 2>&1) || rc=$?
[[ $rc -ne 0 ]] || { echo "FAIL: an absent toolset must not resolve: $out"; exit 1; }
[[ "$out" == *"not installed"* ]] \
    || { echo "FAIL: absent-toolset message: $out"; exit 1; }
# …and it must not have quietly become the default.
out=$("$MCPP" toolchain list 2>&1)
echo "$out" | grep -E '\*\s*msvc' >/dev/null \
    || { echo "FAIL: default was disturbed by a failed pin: $out"; exit 1; }

# 3b) the retired spelling. `msvc@19.x` used to mean "use the system MSVC and
#     verify its banner" — checked here and silently ignored by builds. It now
#     names a toolset, so this machine's own cl version has to say so and
#     point at both replacements.
CLVER=$("$MCPP" toolchain default msvc 2>&1 | grep -oE 'msvc 19\.[0-9]+' | head -1 | cut -d' ' -f2)
if [[ -n "$CLVER" ]]; then
    rc=0; out=$("$MCPP" toolchain default "msvc@$CLVER" 2>&1) || rc=$?
    [[ $rc -ne 0 ]] || { echo "FAIL: msvc@$CLVER should not resolve"; exit 1; }
    [[ "$out" == *"COMPILER version"* ]] \
        || { echo "FAIL: no cl-version signpost: $out"; exit 1; }
    [[ "$out" == *"msvc@system"* ]] \
        || { echo "FAIL: signpost omits msvc@system: $out"; exit 1; }
fi

# 4) mcpp never manages the machine's own Visual Studio
rc=0; out=$("$MCPP" toolchain remove msvc 2>&1) || rc=$?
[[ $rc -ne 0 && "$out" == *"cannot remove it"* ]] \
    || { echo "FAIL: remove msvc: rc=$rc out=$out"; exit 1; }
# …but it must say that a toolset it DID install is removable, or the message
# leaves the reader thinking msvc is simply un-removable.
[[ "$out" == *"msvc@<toolset>"* ]] \
    || { echo "FAIL: remove msvc omits the managed route: $out"; exit 1; }
"$MCPP" toolchain default msvc >/dev/null 2>&1
out=$("$MCPP" toolchain install msvc 2>&1) \
    || { echo "FAIL: install msvc (present) should exit 0: $out"; exit 1; }
[[ "$out" == *"does not manage it"* ]] \
    || { echo "FAIL: install msvc message: $out"; exit 1; }

# 5) native cl.exe builds WORK (0.0.90; the 0.0.88 gate is gone) — the full
#    modules/import-std flow is covered by 99_msvc_native_build.sh; here we
#    assert the basic hello builds and runs under msvc@system.
"$MCPP" new hello_msvc >/dev/null 2>&1
cd hello_msvc
out=$("$MCPP" run 2>&1) || { echo "FAIL: msvc hello build/run: $out"; exit 1; }
[[ "$out" == *"Hello"* || "$out" == *"hello"* ]] \
    || { echo "FAIL: hello output: $out"; exit 1; }

echo "PASS: msvc@system detection, selection, guidance, and native build"
