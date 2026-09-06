#!/usr/bin/env bash
# requires: elf gcc
# A constrained glob whose BACKEND this build never named is left out, not
# refused. A glob whose backend IS named but whose architecture is not covered
# still is.
#
# The refusal is about a real disagreement -- a file written for sm_89 in a
# build targeting sm_80 is not a variant. Across DIFFERENT backends there is no
# disagreement, and refusing there made a build that names a SUBSET of a
# package's backends impossible: a package could have several device backends
# only if every build took all of them. That is the opposite of what an
# additive-backend library needs.
#
# FOUR LEGS, because each states something the others cannot. Leg 2 is the
# change; leg 3 is what must NOT have changed with it; leg 4 is what keeps
# leg 2 from turning a typo into a file that is never compiled.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mk() {   # $1 = the glob's accel, $2 = [package] accelerators list
    rm -rf p; mkdir -p p/src/kernels
    cat > p/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
    cat > p/src/kernels/k.cu <<'EOF'
extern "C" __global__ void k() {}
EOF
    cat > p/mcpp.toml <<EOF
[package]
name         = "subset"
version      = "0.1.0"
accelerators = [$2]
[build]
sources = [
  "src/*.cpp",
  { glob = "src/kernels/*.cu", accel = "$1" },
]
[targets.subset]
kind = "bin"
main = "src/main.cpp"
EOF
    # A build program that claims nothing. Present so the device-source audit
    # has something to report when the glob is NOT excluded -- which is how
    # leg 1 tells "the glob came through" from "the build simply worked".
    cat > p/build.mcpp <<'EOF'
import std;
import mcpp;
int main() { return 0; }
EOF
}

# ── leg 1: the backend is named -- the glob comes through ───────────────────
mk 'cuda12.9+{sm_89}' '"cuda"'
cd p
out=$("$MCPP" build --accel 'cuda12.9+{sm_89}' 2>&1) && { echo "FAIL: the glob did not reach the build"; exit 1; }
echo "$out" | grep -q 'no action compiles' || { echo "FAIL: expected the device-source audit, got:"; echo "$out" | tail -4; exit 1; }
echo "ok: backend named -- the glob is in the build (the audit sees it)"
cd ..

# ── leg 2: a DIFFERENT backend -- the glob is left out ──────────────────────
mk 'cuda12.9+{sm_89}' '"cuda"'
cd p
"$MCPP" build --accel 'vulkan1.2' >leg2.log 2>&1 || {
    echo "FAIL: naming a different backend was refused"
    tail -8 leg2.log
    exit 1
}
echo "ok: a different backend -- the glob is left out, as --no-accel leaves it"
cd ..

# ── leg 3: the SAME backend, an architecture it does not cover -- refused ───
mk 'cuda12.9+{sm_89}' '"cuda"'
cd p
out=$("$MCPP" build --accel 'cuda12.9+{sm_80}' 2>&1) && { echo "FAIL: an uncovered architecture built"; exit 1; }
echo "$out" | grep -q 'does not cover' || { echo "FAIL: expected the accel mismatch refusal, got:"; echo "$out" | tail -4; exit 1; }
echo "ok: same backend, uncovered architecture -- still refused"
cd ..

# ── leg 4: a backend the package never declared -- refused ──────────────────
#
# Without this, leg 2 would turn `accel = "cude12.9"` into a glob that is
# quietly never built and never mentioned.
mk 'cude12.9+{sm_89}' '"cuda"'
cd p
out=$("$MCPP" build --accel 'cuda12.9+{sm_89}' 2>&1) && { echo "FAIL: an undeclared backend built"; exit 1; }
echo "$out" | grep -q 'does not declare' || { echo "FAIL: expected the undeclared-backend refusal, got:"; echo "$out" | tail -4; exit 1; }
echo "$out" | grep -q 'cude' || { echo "FAIL: the refusal does not name the misspelling"; exit 1; }
echo "ok: a backend [package] accelerators does not list -- refused, naming it"

echo "PASS: a subset of backends builds; a mismatch and a misspelling do not"
