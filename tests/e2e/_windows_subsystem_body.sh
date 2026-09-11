# Shared body for the `windows_subsystem` / `windows_entry` e2e tests (#618).
#
# Sourced by 642 (native Windows, MSVC ABI) and 643 (Linux to Windows through
# mingw-cross, GNU ABI). The two ABIs take different flags for one declaration,
# and asserting the same bytes through both keeps that fork honest. Every
# assertion reads the PE optional header's Subsystem field from the linked
# image, so a flag that was spelled but ignored by the linker cannot pass.
#
# Callers set, before sourcing:
#   TMP         scratch directory (created, trap-cleaned)
#   MCPP        the binary under test
#   BUILD_ARGS  extra `mcpp build` arguments ("" natively, --target when cross)
#   RUN_EXE     a command that executes a PE image, or "" when this host has
#               none; criterion 3 is then reported as not measured

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# The Subsystem field of a PE image: e_lfanew at 0x3C, then the 4-byte
# signature and the 20-byte COFF header, then offset 68 of the optional header,
# which is the same in PE32 and PE32+. 2 is WINDOWS_GUI and 3 is WINDOWS_CUI.
pe_subsystem() {
    local lfanew
    lfanew=$(od -An -tu4 -j 60 -N 4 "$1" | tr -d ' ')
    od -An -tu2 -j $((lfanew + 92)) -N 2 "$1" | tr -d ' '
}

mkdir -p "$TMP/proj/src" "$TMP/proj/app"
cd "$TMP/proj"

cat > src/probe.cppm <<'EOF'
export module probe;
export int probe_value() { return 7; }
EOF

# A static constructor and `main` each write a line. With `/ENTRY:main` the CRT
# is never initialised and the constructor line is missing; with the CRT
# startup symbol it precedes the `main` line.
cat > app/gui.cpp <<'EOF'
#include <cstdio>
struct Probe {
    Probe() {
        if (auto f = std::fopen("order.txt", "w")) { std::fputs("ctor\n", f); std::fclose(f); }
    }
};
static Probe probe_instance;
int main() {
    if (auto f = std::fopen("order.txt", "a")) { std::fputs("main\n", f); std::fclose(f); }
    return 0;
}
EOF
printf 'int main() { return 0; }\n' > app/cli.cpp
printf 'int main() { return 0; }\n' > app/tool.cpp
printf 'int wmain() { return 0; }\n' > app/wide.cpp
cat > app/winmain.cpp <<'EOF'
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return 0; }
EOF

# The directive form: the program, not the manifest, chooses `tool`'s subsystem.
cat > build.mcpp <<'EOF'
import mcpp;
int main() {
    mcpp::windows_subsystem("tool", "windows");
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "subsys"
version = "0.1.0"

[modules]
sources = ["src/**/*.cppm"]

[targets.gui]
kind              = "bin"
main              = "app/gui.cpp"
windows_subsystem = "windows"

[targets.cli]
kind = "bin"
main = "app/cli.cpp"

[targets.tool]
kind = "bin"
main = "app/tool.cpp"

[targets.wide]
kind          = "bin"
main          = "app/wide.cpp"
windows_entry = "wmain"

[targets.winmain]
kind              = "bin"
main              = "app/winmain.cpp"
windows_subsystem = "windows"
windows_entry     = "WinMain"
EOF

# shellcheck disable=SC2086
"$MCPP" build $BUILD_ARGS > build.log 2>&1 || fail "the build failed" build.log

exe_of() {
    local p
    p=$(find target -type f -name "$1.exe" -path '*/bin/*' | head -1)
    [ -n "$p" ] || fail "no $1.exe under target/" build.log
    printf '%s' "$p"
}

# Criteria 1 and 2: the declaring executables read 2, the others read 3.
for pair in gui:2 tool:2 winmain:2 cli:3 wide:3; do
    name=${pair%%:*}; want=${pair##*:}
    got=$(pe_subsystem "$(exe_of "$name")")
    [ "$got" = "$want" ] || fail "$name.exe has Subsystem $got, expected $want" build.log
done
echo "subsystem bytes OK (gui, tool, winmain = 2; cli, wide = 3)"

# Criterion 3: the GUI program's static constructor runs before main, and the
# wide entry program starts.
if [ -n "$RUN_EXE" ]; then
    rm -f order.txt
    $RUN_EXE "$(exe_of gui)" > run.log 2>&1 || fail "gui.exe did not exit 0" run.log
    [ "$(tr -d '\r' < order.txt 2>/dev/null)" = "$(printf 'ctor\nmain')" ] \
        || fail "the static constructor did not run before main" order.txt run.log
    $RUN_EXE "$(exe_of wide)" > run.log 2>&1 || fail "wide.exe did not exit 0" run.log
    echo "CRT initialisation OK"
else
    echo "NOT MEASURED: criterion 3, because this host cannot execute a PE image"
fi
