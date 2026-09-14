#!/usr/bin/env bash
# Fetch the released mcpp this record measured (2026.9.14.1) and publish its
# path and its bundled xlings to the job environment. When the job pinned
# MCPP_HOME, the mirror is set here too, in the same process that knows where
# the bundled xlings is: a pinned home is not the tarball's own, and the first
# command in it needs MCPP_VENDORED_XLINGS.
set -euo pipefail
V=2026.9.14.1
if command -v cygpath >/dev/null 2>&1; then RUNNER_TEMP=$(cygpath -u "$RUNNER_TEMP"); fi
case "$(uname -s)" in
    Linux)  asset="mcpp-$V-linux-x86_64.tar.gz";  dir="mcpp-$V-linux-x86_64" ;;
    Darwin) asset="mcpp-$V-macosx-arm64.tar.gz";  dir="mcpp-$V-macosx-arm64" ;;
    MINGW*|MSYS*|CYGWIN*) asset="mcpp-$V-windows-x86_64.zip"; dir="mcpp-$V-windows-x86_64" ;;
    *) echo "unknown host $(uname -s)"; exit 1 ;;
esac
cd "$RUNNER_TEMP"
curl -L -fsS --retry 3 --retry-all-errors -o mcpp.pkg \
    "https://github.com/mcpp-community/mcpp/releases/download/v$V/$asset"
case "$asset" in
    *.zip) unzip -q mcpp.pkg ;;
    *)     tar -xzf mcpp.pkg ;;
esac
MCPP="$RUNNER_TEMP/$dir/bin/mcpp"
[ -f "$MCPP" ] || MCPP="$MCPP.exe"
XL="$RUNNER_TEMP/$dir/registry/bin/xlings"
[ -f "$XL" ] || XL="$XL.exe"
test -f "$MCPP"
test -f "$XL"
if command -v cygpath >/dev/null 2>&1; then XL=$(cygpath -m "$XL"); fi
export MCPP_VENDORED_XLINGS="$XL"
"$MCPP" --version
if [ -n "${MCPP_HOME:-}" ]; then
    "$MCPP" self config --mirror GLOBAL
fi
echo "MCPP=$MCPP" >> "$GITHUB_ENV"
echo "MCPP_VENDORED_XLINGS=$XL" >> "$GITHUB_ENV"
