#!/usr/bin/env bash
# 84_runtime_dll_deploy.sh — Windows runtime-DLL deployment, validated MECHANICALLY
# on any host. A dependency's `[runtime] library_dirs` may hold a runtime DLL that
# a directly-launched executable must find beside it (Windows has no RPATH). mcpp
# stages every *.dll from a linked dependency's runtime library_dir into bin/,
# next to the produced executable.
#
# The deploy is filtered by the *.dll extension, NOT by `if constexpr(is_windows)`:
# a real Linux/macOS dependency ships .so/.dylib (never .dll), so the mechanism is
# inert there and non-Windows builds are byte-for-byte unchanged. This test ships a
# dummy `libdummy.dll` so the exact copy-beside-exe path is exercised on the Linux
# CI host — the Windows link/run half is covered by mcpp-index CI (Phase D).
# See .agents/docs/2026-06-29-windows-runtime-dll-deployment-and-openblas.md.
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A "prebuilt" dependency whose runtime artifact is a DLL sitting in bin/. The
# file content is irrelevant — deployment is a file copy, not a link — so a stub
# byte stream named libdummy.dll stands in for a real OpenBLAS-style DLL.
mkdir -p blasish/bin blasish/src
cat > blasish/mcpp.toml <<'EOF'
[package]
name    = "blasish"
version = "0.1.0"

[targets.blasish]
kind = "lib"

# The runtime DLL lives in bin/. On Windows mcpp copies it beside the consumer's
# .exe; on RPATH platforms the *.dll filter makes this a no-op.
[runtime]
library_dirs = ["bin"]
EOF
cat > blasish/src/blasish.cppm <<'EOF'
export module blasish;
export int blasish_anchor() { return 0; }
EOF
# Stub runtime DLL (and a sibling non-DLL that must NOT be deployed).
printf 'MZ stub dll payload' > blasish/bin/libdummy.dll
printf 'not a dll'           > blasish/bin/readme.txt

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
blasish = { path = "../blasish" }
EOF
cat > app/src/main.cpp <<'EOF'
import blasish;
int main() { return blasish_anchor(); }
EOF

cd app
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build errored"; exit 1; }

# The executable's output directory (bin/, beside the .exe).
BIN=$(find target -type f \( -name app -o -name app.exe \) | head -1)
[ -n "$BIN" ] || { echo "FAIL: built binary not found under target/"; cat b.log; exit 1; }
BINDIR=$(dirname "$BIN")

# The dependency's runtime DLL must have been staged beside the executable.
if [ ! -f "$BINDIR/libdummy.dll" ]; then
    echo "FAIL: libdummy.dll was not deployed beside the executable ($BINDIR)"
    echo "--- bin/ contents ---"; ls -la "$BINDIR"
    echo "--- build.ninja deploy edges ---"; grep -n "libdummy" target/*/*/build.ninja 2>/dev/null || true
    exit 1
fi

# Byte-for-byte copy of the source DLL.
cmp -s ../blasish/bin/libdummy.dll "$BINDIR/libdummy.dll" || {
    echo "FAIL: deployed libdummy.dll differs from source"; exit 1; }

# The non-DLL sibling must NOT be deployed (only *.dll is staged).
if [ -f "$BINDIR/readme.txt" ]; then
    echo "FAIL: non-DLL readme.txt was deployed (only *.dll should be)"; exit 1; fi

echo "OK"
