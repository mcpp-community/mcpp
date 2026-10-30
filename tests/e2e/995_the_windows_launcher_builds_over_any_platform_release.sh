#!/usr/bin/env bash
# requires: llvm
# 995 -- the Windows launcher of a package is mcpp's program, built over the
# platform mcpp's own openkal rows pin, whichever openkal-platform release the
# project selects (SPEC-014 §4.3; docs/10). The launcher's mcpplibs.os pins one
# openkal release; built over the project's platform, a version group naming
# another openkal put two of them in its graph and `mcpp pack` stopped
# (openkal-platform 0.19.0's CI, 2026-10-30).
#
# Criteria:
#   A. A project that selects openkal-platform 0.19.0 (openkal 0.18.0) packs
#      `--mode self-contained --systems linux,windows`: the tree holds
#      `bin/<name>.exe` and the Windows runner.
#   B. The image is the selected platform's: the pack names 0.19.0.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64|Linux-amd64) ;;
    *) echo "SKIP: the Windows launcher is built here on Linux x86_64"; exit 0 ;;
esac

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "wl"
version = "0.1.0"

[target.'cfg(os = "openkal")']
platform = "mcpplibs.openkal-platform@0.19.0"
TOML
cat > src/main.cpp <<'CPP'
import std;
int main(int argc, char**) { std::println("wl ok: {} args", argc); }
CPP

# A
"$MCPP" pack --target x86_64-openkal --mode self-contained --systems linux,windows \
    --pkg-format dir -o t > a.log 2>&1 || fail "A: pack" a.log
[ -f target/dist/t/bin/wl.exe ] || fail "A: no bin/wl.exe" a.log
[ -f target/dist/t/libexec/okrun/windows-x86_64/okrun.exe ] || fail "A: no Windows runner" a.log
echo "ok: the launcher builds whichever platform release the project selects"

# B
grep -q "openkal-platform 0.19.0" a.log || fail "B: the selected platform is not the image's" a.log
echo "ok: the image is the selected platform's"

echo "PASS: 995 the Windows launcher builds over any platform release"
