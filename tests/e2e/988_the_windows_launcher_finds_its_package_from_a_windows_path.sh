#!/usr/bin/env bash
# requires: llvm python3 wine
# 988 -- the Windows launcher finds its package from a Windows path (SPEC-014
# §4.3; design 2026-10-29 rev 2 §4.3).
#
# `bin/<name>.exe` is built over the platform's hosted arrangement, whose paths
# are POSIX's. Windows names the program with `\` (`D:\a\…\bin\app.exe`), which
# such a path does not split, so the launcher took an empty parent as the
# package and said "the package holds no architecture" with `lib/x86_64/` in
# place (openkal-platform CI on windows-2022, 2026-10-30). The launcher now
# reads its own path with `/` between the names.
#
# Criteria:
#   A. `mcpp pack --target x86_64-openkal --mode self-contained --systems
#      linux,windows` writes `bin/<name>.exe` and the Windows runner.
#   B. Under wine, which names the program `Z:\…\bin\<name>.exe`, the launcher
#      runs the image through the carried runner with its arguments.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64|Linux-amd64) ;;
    *) echo "SKIP: the Windows runner is x86_64 and wine runs it on Linux x86_64"; exit 0 ;;
esac

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "wl"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
int main(int argc, char**) { std::println("wl ok: {} args", argc); }
CPP

# A
"$MCPP" pack --target x86_64-openkal --mode self-contained --systems linux,windows \
    --pkg-format dir -o t > a.log 2>&1 || fail "A: pack" a.log
[ -f target/dist/t/bin/wl.exe ] || fail "A: no bin/wl.exe" a.log
[ -f target/dist/t/libexec/okrun/windows-x86_64/okrun.exe ] || fail "A: no Windows runner" a.log
echo "ok: the tree holds the Windows launcher and runner"

# B
WINEDEBUG=-all timeout 120 wine target/dist/t/bin/wl.exe one two 2>&1 | tr -d '\r' > b.log || true
grep -q "wl ok: 3 args" b.log || fail "B: the launcher under wine" b.log
echo "ok: the launcher finds its package from a Windows path"

echo "PASS: 988 the Windows launcher finds its package from a Windows path"
