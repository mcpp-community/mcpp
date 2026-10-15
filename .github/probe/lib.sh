# Shared helpers for the probe scripts. Every script prints what it observes
# and never asserts: a failing command is recorded with its exit code.
XV="${XV:-2026.10.15.4}"     # the xlings that installs the "upstream" home
MV="${MV:-2026.10.11.1}"     # the published mcpp under test
T="${T:-$RUNNER_TEMP/p}"
mkdir -p "$T" "$T/home" "$T/logs"
SUMMARY="${GITHUB_STEP_SUMMARY:-/dev/null}"

say() { echo "$*"; echo "$*" >> "$SUMMARY"; }
section() { say ""; say "### $*"; }
# Run a command, print its (tail of) output into the summary as a code block.
obs() {
    local title="$1"; shift
    local out rc
    out="$("$@" 2>&1)"; rc=$?
    echo "== $title (exit $rc)"; echo "$out"
    {
        echo ""; echo "**$title** (exit $rc)"; echo '```'
        echo "$out" | tail -n "${OBS_LINES:-25}"
        echo '```'
    } >> "$SUMMARY"
    return 0
}
clean_env() {
    env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE -u XLINGS_SUBOS_LIB -u XLINGS_BIN \
        -u XLINGS_PROJECT_DIR -u XLINGS_SHIM_DEPTH -u XLINGS_HOME -u MCPP_VENDORED_XLINGS "$@"
}
# An ELF's loader and run path, one line each.
elf() {
    local f="$1"
    [ -e "$f" ] || { echo "$f: absent"; return; }
    echo "$f"
    readelf -l "$f" 2>/dev/null | grep -o 'interpreter: [^]]*' | sed 's/^/  /'
    readelf -d "$f" 2>/dev/null | grep -E 'RUNPATH|RPATH' | sed 's/.*\[/  runpath [/'
}
# A Mach-O's dylibs and rpaths.
macho() {
    local f="$1"
    [ -e "$f" ] || { echo "$f: absent"; return; }
    echo "$f"
    otool -L "$f" 2>/dev/null | tail -n +2 | sed 's/^/  dylib/'
    otool -l "$f" 2>/dev/null | awk '/LC_RPATH/{r=1} r&&/path /{print "  rpath " $2; r=0}'
}
# Digest of every regular file under a directory (to show it was not written).
tree_digest() {
    (cd "$1" && find . -type f -print0 | sort -z | xargs -0 sha256sum 2>/dev/null | sha256sum | cut -c1-16)
}
hello_cpp() {
    cat > "$1" <<'EOF'
#include <cstdio>
#include <string>
int main() { std::string s = "hello from the probe"; std::puts(s.c_str()); return 0; }
EOF
}
