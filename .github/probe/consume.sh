#!/usr/bin/env bash
# TEMPORARY PROBE (do not merge): consume the published mcpp 2026.10.8.1 from
# a fresh xlings home with the published xlings client, through one mirror,
# and build and run a project with the host's default toolchain.
set -euo pipefail
V=2026.10.8.1; XV=2026.10.8.1; MIRROR=$1
base="$RUNNER_TEMP/consume-$MIRROR"; rm -rf "$base"; mkdir -p "$base"; cd "$base"
case "$(uname -s)-$(uname -m)" in
  Linux-aarch64) a=xlings-$XV-linux-aarch64.tar.gz ;;
  Linux-x86_64)  a=xlings-$XV-linux-x86_64.tar.gz ;;
  Darwin-arm64)  a=xlings-$XV-macosx-arm64.tar.gz ;;
  *)             a=xlings-$XV-windows-x86_64.zip ;;
esac
curl -fsSL -o "$a" "https://github.com/openxlings/xlings/releases/download/v$XV/$a"
case "$a" in *.zip) unzip -q "$a" ;; *) tar xzf "$a" ;; esac
xl=$(find "$base" -type f \( -name xlings -o -name xlings.exe \) -path '*/bin/*' | head -1)
export XLINGS_HOME="$base/home" XLINGS_NON_INTERACTIVE=1
"$xl" --version
"$xl" config --mirror "$MIRROR"
"$xl" update
mkdir -p "$base/neutral"; cd "$base/neutral"
"$xl" install mcpp -y
m=$(find "$XLINGS_HOME/data/xpkgs/xim-x-mcpp" -maxdepth 2 \( -name mcpp -o -name mcpp.exe -o -name mcpp.cmd -o -name mcpp.bat \) | head -1)
echo "installed: $(ls "$XLINGS_HOME/data/xpkgs/xim-x-mcpp")"
export MCPP_HOME="$base/mcpp-home"
"$m" --version | tee version.txt
grep -q "mcpp $V" version.txt
"$m" new hello
cd hello
"$m" run 2>&1 | tee run.log
grep -q "Hello from hello" run.log
"$m" toolchain list 2>&1 | head -20 || true
echo "PASS: $(uname -s)-$(uname -m) $MIRROR consumed mcpp $V and ran hello"
