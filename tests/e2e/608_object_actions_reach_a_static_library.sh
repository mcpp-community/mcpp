#!/usr/bin/env bash
# requires: gcc
# An `object`-role action's outputs join a STATIC LIBRARY, not only an
# executable. That is the engine half of the multi-device design's C-6: a
# package whose device code is its point declares `kind = "lib"`, and until
# this the actions it emitted were dropped with a warning and the archive came
# out with none of them in it.
#
# Nothing here names a device. The "device compiler" is the toolchain's own C
# compiler and the property under test is which link units an action attaches
# to, which is not a vendor question.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new archived > /dev/null; cd archived
rm -f src/*.cppm src/main.cpp
mkdir -p src
cat > src/host.c <<'EOF2'
extern int from_action(void);
int host_value(void) { return from_action(); }
EOF2
cat > mcpp.toml <<'EOF2'
[package]
name = "archived"
version = "0.1.0"
[language]
standard = "c++23"
[build]
sources = ["src/*.c"]
[targets.archived]
kind = "lib"
EOF2
cat > build.mcpp <<'EOF2'
import std;
import mcpp;
int main() {
    const std::string out  = std::string(mcpp::out_dir());
    const std::string src  = out + "/piece.c";
    { std::ofstream f(src, std::ios::trunc);
      f << "int from_action(void) { return 7; }\n"; }
    const std::string obj = out + "/piece.o";
    mcpp::action a;
    a.id = "piece"; a.role = "object"; a.description = "compile the piece";
    a.arg((std::string(mcpp::toolchain_dir()) + "/bin/gcc").c_str());
    a.arg("-c"); a.arg(src.c_str()); a.arg("-o"); a.arg(obj.c_str());
    a.input(src.c_str());
    a.output(obj.c_str());
    a.submit();
    return 0;
}
EOF2

# ── One: the action runs and its object is IN the archive ────────────────
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "FAIL: the build failed"; exit 1; }

grep -q "produces no target" build.log && {
    cat build.log
    echo "FAIL: the action was dropped for want of a linked image"
    exit 1
}

lib=$(find target -name 'libarchived.a' | head -1)
[ -n "$lib" ] || { echo "FAIL: no static library was produced"; exit 1; }

# THE MEMBER LIST IS THE ASSERTION, not the exit status: an `ar` that was
# handed nothing still writes a well-formed archive and reports success.
members=$(ar t "$lib" 2>/dev/null | tr '\n' ' ')
case "$members" in
    *piece.o*) ;;
    *) echo "FAIL: piece.o is not a member of the archive (members: $members)"; exit 1 ;;
esac
echo "PASS: an object action's output is archived into a static library"

# ── Two: the symbol is really there ─────────────────────────────────────
if command -v nm > /dev/null 2>&1; then
    nm "$lib" 2>/dev/null | grep -q "from_action" || {
        echo "FAIL: the archive has the member but not its symbol"; exit 1; }
    echo "PASS: the archived member carries its symbol"
fi

echo "PASS: object actions reach a static library"
