#!/usr/bin/env bash
# requires: gcc
# A SYCL translation unit is a device source, and the criterion for that is the
# COMPILER rather than the dialect.
#
# WHAT THIS MEASURES. `SourceKind::Device` is documented as a graph role -- "not
# scanned, no BMI, compiled by a device compiler mcpp does not drive" -- and
# every extension in the table until now was a dialect a general C++ compiler
# would refuse. `.sycl` is not: its content is ordinary C++. What makes it a
# device unit is that it goes to a second compiler with a device back end
# (icpx, or a clang built with the SYCL front end), which mcpp does not drive
# and which does not accept C++20 modules.
#
# So this file asserts the row exists, that adding it changed nothing else, and
# -- the part a test naming `.sycl` alone would miss -- that the C++ spelling of
# the same content is still C++.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new sycltest > /dev/null; cd sycltest
rm -f src/*.cppm
cat > src/main.cpp <<'EOF2'
int main() { return 0; }
EOF2
mkdir -p src/kernels

# The build program prints what it was handed, delimited, so a section can
# assert on a whole list rather than on a substring of one name. Delimited, so
# an empty list is `device=[]` and not the absence of a line -- which is what
# section three asserts on.
cat > build.mcpp <<'EOF2'
import std;
import mcpp;
int main() {
    std::string flat(mcpp::device_sources());
    for (auto& c : flat) if (c == '\n') c = ' ';
    mcpp::warning(("device=[" + flat + "]").c_str());
    return 0;
}
EOF2

write_manifest() {   # $1 = accel line, $2 = sources line
    cat > mcpp.toml <<EOF2
[package]
name = "sycltest"
version = "0.1.0"
[language]
standard = "c++23"

[build]
$1
sources = [$2]

[targets.sycltest]
kind = "bin"
main = "src/main.cpp"
EOF2
}

# ── One: a .sycl unit in a constrained glob reaches the build program ─────
cat > src/kernels/saxpy.sycl <<'EOF2'
// Ordinary C++. What makes this a device unit is which compiler consumes it.
extern "C" int saxpy_device(int n) { return n; }
EOF2

write_manifest 'accel   = "sycl"' '"src/*.cpp", { glob = "src/kernels/*.sycl", accel = "sycl" }'
"$MCPP" build > one.log 2>&1 || { cat one.log; echo "FAIL: a project with a .sycl source failed to build"; exit 1; }
grep -q "device=\[.*src/kernels/saxpy.sycl.*\]" one.log || {
    cat one.log; echo "FAIL: the .sycl unit did not reach MCPP_DEVICE_SOURCES"; exit 1; }
echo "PASS: a .sycl unit in a constrained glob reaches the build program"

# ── Two: and it is not offered to the C++ compiler ────────────────────────
#
# The complement of section one, and the section that would fail if `.sycl`
# had been added as an ordinary C++ extension instead of a device one: the
# unit would reach the build program AND be compiled, producing an object
# built by the wrong compiler that links and silently contains no device code.
"$MCPP" build -v > two.log 2>&1
if grep -qE "saxpy\.sycl -o|saxpy\.sycl\.o" two.log; then
    cat two.log; echo "FAIL: the .sycl unit was offered to the C++ compiler"; exit 1
fi
echo "PASS: the .sycl unit is not compiled by mcpp's own compiler"

# ── Three: the SAME CONTENT named .cpp is still C++ ───────────────────────
#
# The control, and the one section that separates "the table gained a row" from
# "the table now classifies by content". A project with a `.cpp` under the same
# directory must compile it and must NOT hand it to the build program.
rm -f src/kernels/saxpy.sycl
cp /dev/null src/kernels/saxpy.cpp
cat > src/kernels/saxpy.cpp <<'EOF2'
extern "C" int saxpy_device(int n) { return n; }
EOF2
write_manifest 'accel   = "sycl"' '"src/*.cpp", "src/kernels/*.cpp"'
"$MCPP" build -v > three.log 2>&1 || { cat three.log; echo "FAIL: the .cpp spelling failed to build"; exit 1; }
grep -q "device=\[\]" three.log || {
    cat three.log; echo "FAIL: a .cpp was handed to the build program as a device source"; exit 1; }
grep -qE "saxpy\.cpp" three.log || {
    cat three.log; echo "FAIL: the .cpp was not compiled"; exit 1; }
echo "PASS: the same content named .cpp is compiled as C++ and is not a device source"

# ── Four: --no-accel takes the constrained glob out ───────────────────────
#
# The variant switch, applied to the new row. A device source carries the accel
# it is for, so a build that asks for none must not see it -- and this is what
# makes the CPU half of a seam reachable without editing the manifest. Asserted
# as an empty list rather than as the absence of a name, so a build program that
# printed nothing at all would fail here.
# Section three left a `.cpp` in that directory and no `.sycl`; put the device
# unit back so the glob has something to exclude.
rm -f src/kernels/saxpy.cpp
cat > src/kernels/saxpy.sycl <<'EOF2'
extern "C" int saxpy_device(int n) { return n; }
EOF2
write_manifest 'accel   = "sycl"' '"src/*.cpp", { glob = "src/kernels/*.sycl", accel = "sycl" }'
"$MCPP" build --no-accel > four.log 2>&1 || { cat four.log; echo "FAIL: --no-accel failed to build"; exit 1; }
grep -q "device=\[\]" four.log || {
    cat four.log; echo "FAIL: --no-accel still handed the .sycl unit to the build program"; exit 1; }
echo "PASS: --no-accel removes the constrained glob that carries the .sycl unit"

# ── Five: the default source globs did not widen ──────────────────────────
#
# The other half of the compatibility promise. A package that vendors SYCL
# sources it builds elsewhere must not start handing them to a build program on
# upgrade, so a project with NO `sources` entry must see none of them.
rm -f mcpp.toml
cat > mcpp.toml <<'EOF2'
[package]
name = "sycltest"
version = "0.1.0"
[language]
standard = "c++23"

[targets.sycltest]
kind = "bin"
main = "src/main.cpp"
EOF2
"$MCPP" build > five.log 2>&1 || { cat five.log; echo "FAIL: the default-glob project failed to build"; exit 1; }
grep -q "device=\[\]" five.log || {
    cat five.log; echo "FAIL: a default glob picked up the .sycl source"; exit 1; }
echo "PASS: the default source globs do not include .sycl"

echo "ALL PASS: 613"
