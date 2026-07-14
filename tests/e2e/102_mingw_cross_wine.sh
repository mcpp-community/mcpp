#!/usr/bin/env bash
# requires: mingw-cross wine
# Linux → Windows MinGW cross: build a multi-module + `import std` project for
# x86_64-w64-mingw32, verify the PE is statically self-contained, run it under
# wine. Realizes Part C of 2026-07-15-mingw-linux-cross-windows-design.md.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new crosswin
cd crosswin

# A named module that pulls in `import std` — exercises the cross libstdc++
# std module (bits/std.cc built for the mingw target).
cat > src/banner.cppm <<'EOF'
export module crosswin.banner;
import std;
export auto banner() -> void {
    std::println("hello from x86_64-w64-mingw32, running under wine");
}
EOF

cat > src/main.cpp <<'EOF'
import std;
import crosswin.banner;
int main() {
    banner();
    std::println("mingw-cross import-std + multi-module OK");
    return 0;
}
EOF

TRIPLE=x86_64-w64-mingw32

# ── build for the Windows PE target ────────────────────────────────────────
"$MCPP" build --target "$TRIPLE" > "$TMP/build.log" 2>&1 || {
    echo "cross build failed:"; cat "$TMP/build.log"; exit 1; }

EXE="$(find target/$TRIPLE -name '*.exe' -type f | head -1)"
[[ -n "$EXE" && -f "$EXE" ]] || { echo "no .exe produced under target/$TRIPLE"; exit 1; }

# PE sanity: must be a PE32+ console binary.
file "$EXE" | grep -q "PE32+" || { echo "artifact is not PE32+: $(file "$EXE")"; exit 1; }

# ── static self-containment: no C++/gcc/pthread runtime DLLs in the import
#    table (the clean path for wine — nothing to copy into the prefix). Only
#    the OS DLLs (kernel32/msvcrt/user32…) are allowed. ─────────────────────
OBJDUMP="$(dirname "$(command -v x86_64-w64-mingw32-g++ 2>/dev/null || \
    find "$HOME/.xlings" "${MCPP_HOME:-$HOME/.mcpp}" -name 'x86_64-w64-mingw32-g++' 2>/dev/null | head -1)")/x86_64-w64-mingw32-objdump"
if [[ -x "$OBJDUMP" ]]; then
    if "$OBJDUMP" -p "$EXE" | grep -iE "DLL Name: (libstdc\+\+|libgcc|libwinpthread)" ; then
        echo "artifact dynamically depends on a runtime DLL — expected fully static"
        exit 1
    fi
fi

# ── run under wine in an isolated prefix (deleted on exit) ──────────────────
export WINEPREFIX="$TMP/wineprefix" WINEDEBUG=-all
out="$(wine "$EXE" 2>/dev/null)"
[[ "$out" == *"running under wine"* ]]          || { echo "banner() output missing: $out"; exit 1; }
[[ "$out" == *"import-std + multi-module OK"* ]] || { echo "main output missing: $out"; exit 1; }

# ── ninja sanity: std BMI + module BMI were built for the cross target ──────
build_ninja="$(find target/$TRIPLE -name build.ninja | head -1)"
grep -q "gcm.cache/std.gcm"               "$build_ninja" || { echo "ninja missing std BMI"; exit 1; }
grep -q "gcm.cache/crosswin.banner.gcm"   "$build_ninja" || { echo "ninja missing module BMI"; exit 1; }

echo "OK"
