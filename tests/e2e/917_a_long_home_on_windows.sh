#!/usr/bin/env bash
# requires: windows
# 917 -- Windows long paths (docs/25 §6).
#
# Criteria:
#   A. mcpp.exe declares itself long-path aware in its application manifest.
#   B. With long paths on (LongPathsEnabled = 1), an MCPP_HOME 157 characters
#      long builds and runs a program; with them off, mcpp reports the path
#      budget as a degradation naming both remedies.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

# ── A ─────────────────────────────────────────────────────────────────────────
grep -a -q '<longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>' "$MCPP" \
    || fail "A: mcpp.exe does not declare longPathAware"
echo "ok: A. mcpp.exe is long-path aware"

# ── B ─────────────────────────────────────────────────────────────────────────
LONG="$TMP"
while [ ${#LONG} -lt 150 ]; do LONG="$LONG/deep-directory"; done
LONG="${LONG:0:157}"
export MCPP_HOME="$LONG"
mkdir -p "$MCPP_HOME"
enabled=$(reg query 'HKLM\SYSTEM\CurrentControlSet\Control\FileSystem' //v LongPathsEnabled 2>/dev/null | grep -o '0x[0-9a-f]*' || echo 0x0)
if [ "$enabled" = "0x1" ]; then
    source "$(dirname "$0")/_inherit_toolchain.sh"
    cd "$TMP"
    "$MCPP" new hello > new.log 2>&1 || fail "B: mcpp new" new.log
    cd hello
    "$MCPP" build > build.log 2>&1 || fail "B: a build under a 157-character MCPP_HOME failed" build.log
    "$MCPP" run > run.log 2>&1 || fail "B: the program does not run" run.log
    echo "ok: B. a 157-character MCPP_HOME builds with long paths on"
else
    "$MCPP" self env > env.log 2>&1 || true
    grep -q "Windows long paths are off" env.log || fail "B: the path budget is not reported" env.log
    grep -q "shorter MCPP_HOME" env.log || fail "B: the report does not name the remedies" env.log
    echo "ok: B. with long paths off, the path budget is reported"
fi

echo "PASS: 917_a_long_home_on_windows"
