#!/usr/bin/env bash
# requires: elf gcc
# A device-kind source that no action compiles is refused, and the refusal
# names the file.
#
# The engine has no compile rule for `.cu` and never will: a device source is
# handed to the package's build program and comes back as an action, or it is
# not compiled at all. Nothing checked that it came back, so a project whose
# build program does not claim the extension -- or that has no build program --
# got an undefined reference at the link naming a SYMBOL and never the file
# that would have defined it. For a `kind = "lib"` target not even that,
# because an archive is not resolved.
#
# TWO LEGS, because the two situations have different fixes and the message
# has to distinguish them.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mk() {
    rm -rf p; mkdir -p p/src/kernels
    cat > p/src/main.cpp <<'EOF'
extern "C" void k();
int main() { return 0; }
EOF
    cat > p/src/kernels/k.cu <<'EOF'
extern "C" __global__ void k() {}
EOF
    cat > p/mcpp.toml <<'EOF'
[package]
name    = "orphan"
version = "0.1.0"
accelerators = ["cuda"]
[build]
accel = "cuda12.9+{sm_89}"
sources = [
  "src/*.cpp",
  { glob = "src/kernels/*.cu", accel = "cuda12.9+{sm_89}" },
]
[targets.orphan]
kind = "bin"
main = "src/main.cpp"
EOF
}

# ── leg 1: no build program at all ──────────────────────────────────────────
mk
cd p
out=$("$MCPP" build 2>&1) && { echo "FAIL: a package with an uncompilable device source built"; exit 1; }
echo "$out" | grep -q 'src/kernels/k.cu' || { echo "FAIL: the refusal does not name the file"; echo "$out" | tail -5; exit 1; }
echo "$out" | grep -q 'no `build.mcpp`' || { echo "FAIL: the refusal does not say the build program is missing"; echo "$out" | tail -5; exit 1; }
echo "ok: no build program -- named the file and the missing program"
cd ..

# ── leg 2: a build program that claims nothing ──────────────────────────────
#
# The ordinary shape for a project with two backends: a rule takes the
# extensions it knows and leaves the rest, so a file no imported rule claims is
# left over. Distinguished from leg 1 because the fix is different.
mk
cat > p/build.mcpp <<'EOF'
import std;
import mcpp;
int main() { return 0; }
EOF
cd p
out=$("$MCPP" build 2>&1) && { echo "FAIL: an unclaimed device source built"; exit 1; }
echo "$out" | grep -q 'src/kernels/k.cu' || { echo "FAIL: the refusal does not name the file"; echo "$out" | tail -5; exit 1; }
echo "$out" | grep -q 'ran but declared no action' || { echo "FAIL: the refusal does not distinguish a program that claimed nothing"; echo "$out" | tail -5; exit 1; }
echo "ok: build program present -- named the file and said no action took it"
cd ..

# ── the negative leg: --no-accel leaves the glob out, so there is nothing to
# refuse. Without this the two legs above would pass on an engine that refused
# EVERY device source, which is a different and wrong behaviour.
mk
cd p
"$MCPP" build --no-accel >/dev/null 2>&1 || { echo "FAIL: --no-accel must not be refused; the constrained glob is left out"; exit 1; }
echo "ok: --no-accel builds -- no device source, nothing to account for"

echo "PASS: a device source that reaches no action is refused, and only then"
