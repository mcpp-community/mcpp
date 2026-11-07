#!/usr/bin/env bash
# requires: msvc
# cl.exe OBJECTS CARRY THEIR OWN DEBUG INFORMATION (2026.11.7.1).
# A dev build compiled with `/Zi /FS`: every compile of a directory wrote one
# PDB through mspdbsrv.exe, which the first cl.exe started and which outlives
# it. Each compile runs in a process unit that retires with it, so retiring
# the unit that held the server ended it under the concurrent compiles, and
# one failed with C1090, PDB API error 23 (258, intermittently). Debug
# information is now `/Z7`, in each object, and a shared-PDB word in the
# manifest is refused before anything compiles.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
mkdir -p "$TMP/many/src"
cd "$TMP/many"
cat > mcpp.toml <<'EOF2'
[package]
name    = "many"
version = "0.1.0"
[toolchain]
windows = "msvc@system"
EOF2
# Enough units that compiles overlap, each with debug information.
for k in $(seq 1 16); do
    printf 'int part%d(int v) { return v + %d; }\n' "$k" "$k" > "src/part$k.cpp"
done
{
    for k in $(seq 1 16); do printf 'int part%d(int);\n' "$k"; done
    printf 'int main() { int s = 0;\n'
    for k in $(seq 1 16); do printf '    s += part%d(0);\n' "$k"; done
    printf '    return s == 136 ? 0 : 1; }\n'
} > src/main.cpp

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: a dev build of overlapping cl.exe compiles did not build"; exit 1; }
nj="$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)"
grep -q '/Z7' "$nj" || {
    grep -n 'cxxflags\|/Z' "$nj" | head
    echo "FAIL: the dev profile's cl.exe compile does not write /Z7 debug information"; exit 1; }
if grep -qE '(^| )/(Zi|FS)( |$)' "$nj"; then
    grep -nE '/(Zi|FS)' "$nj" | head
    echo "FAIL: a cl.exe compile still writes a shared PDB"; exit 1
fi
"$MCPP" run > run.log 2>&1 || {
    cat run.log; echo "FAIL: the program of the dev build did not run"; exit 1; }

# A shared-PDB word in the manifest is refused before anything compiles.
cat >> mcpp.toml <<'EOF2'
[build]
cxxflags = ["/Zi"]
EOF2
if "$MCPP" build > refused.log 2>&1; then
    cat refused.log
    echo "FAIL: [build] cxxflags = [\"/Zi\"] built on cl.exe"; exit 1
fi
grep -q 'mspdbsrv' refused.log && grep -q '/Z7' refused.log || {
    cat refused.log
    echo "FAIL: the refusal does not say why /Zi is refused and what to write"; exit 1; }
if grep -q 'Compiling' refused.log; then
    cat refused.log; echo "FAIL: the refusal came after compiling began"; exit 1
fi
echo "PASS: cl.exe objects carry their own debug information; /Zi is refused"
