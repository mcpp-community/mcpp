#!/usr/bin/env bash
# requires: gcc
# A `[build] sources` entry may carry the accel it is for, and the build
# narrows to it.
#
#   sources = ["src/**/*.cpp", { glob = "src/dev/**/*", accel = "widget9+{w1}" }]
#
# Three outcomes, each measured here with its control:
#   - the build covers the constraint: the glob's C++ files compile, and its
#     device-kind files reach the build program as MCPP_DEVICE_SOURCES;
#   - `--no-accel`: the glob is left out -- its C++ file is not compiled and
#     the device list is empty. This is the CPU-only variant of one project;
#   - the build targets something else: refused, naming the glob and both
#     sides (`accel-mismatch`);
#   - a constrained glob matching no file: refused, naming the glob.
#
# `widget` is not a backend anything knows. The engine reads a shape.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new narrow > /dev/null; cd narrow
rm -f src/*.cppm; mkdir -p src/dev
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF
# A C++ file under the constrained glob: whether it is compiled is visible in
# the verbose command lines. A device-kind file beside it: the engine has no
# rule for it, and hands it to the build program.
printf 'int widget_helper() { return 42; }\n' > src/dev/helper.cpp
printf '__global__ void k() {}\n'             > src/dev/kernel.cu

write_manifest() {   # $1 = accel of the build, $2 = accel of the glob, $3 = glob
    cat > mcpp.toml <<EOF
[package]
name = "narrow"
version = "0.1.0"
[language]
standard = "c++23"

[build]
accel   = "$1"
sources = ["src/*.cpp", { glob = "$3", accel = "$2" }]

[targets.narrow]
kind = "bin"
main = "src/main.cpp"
EOF
    cat > build.mcpp <<'EOF'
import mcpp;
int main() {
    const char* d = mcpp::device_sources();
    mcpp::warning((*d ? d : "(no device sources)"));
    return 0;
}
EOF
}

# ── One: the build covers the constraint ─────────────────────────────────
write_manifest "widget9+{w1,w2}" "widget9+{w1}" "src/dev/**/*"
"$MCPP" build -v > covered.log 2>&1 || { cat covered.log; echo "FAIL: a covered constraint failed the build"; exit 1; }
grep -q "helper.cpp" covered.log || { cat covered.log; echo "FAIL: the constrained glob's C++ file was not compiled"; exit 1; }
grep -q "src/dev/kernel.cu" covered.log || { cat covered.log; echo "FAIL: the device source did not reach the build program"; exit 1; }
echo "PASS: a covered constraint compiles its C++ and hands its device sources to the build program"

# ── Two: --no-accel leaves the glob out ───────────────────────────────────
"$MCPP" build -v --no-accel > none.log 2>&1 || { cat none.log; echo "FAIL: --no-accel failed the build"; exit 1; }
if grep -q "helper.cpp" none.log; then
    cat none.log; echo "FAIL: the constrained glob was compiled under --no-accel"; exit 1
fi
grep -q "(no device sources)" none.log || { cat none.log; echo "FAIL: device sources were listed under --no-accel"; exit 1; }
echo "PASS: --no-accel leaves the constrained glob out, C++ and device files alike"

# ── Three: the build targets something the constraint is not within ──────
write_manifest "widget9+{w2}" "widget9+{w1}" "src/dev/**/*"
if "$MCPP" build > mismatch.log 2>&1; then
    cat mismatch.log; echo "FAIL: a constraint outside the build's accel was accepted"; exit 1
fi
grep -q "src/dev/\*\*/\*" mismatch.log || { cat mismatch.log; echo "FAIL: refusal does not name the glob"; exit 1; }
grep -q "widget9+{w1}" mismatch.log   || { cat mismatch.log; echo "FAIL: refusal does not name the constraint"; exit 1; }
grep -q "widget9+{w2}" mismatch.log   || { cat mismatch.log; echo "FAIL: refusal does not name what the build targets"; exit 1; }
reason="$("$MCPP" why toolchain --format json 2>/dev/null | jq -r '.data.reason // "-"' | tr -d '\r')"
[[ "$reason" == "accel-mismatch" ]] || { echo "FAIL: reason is '$reason', expected accel-mismatch"; exit 1; }
echo "PASS: a constraint outside the build is refused naming the glob and both sides"

# ── Four: a constrained glob that matches nothing ─────────────────────────
write_manifest "widget9+{w1,w2}" "widget9+{w1}" "src/nowhere/**/*"
if "$MCPP" build > empty.log 2>&1; then
    cat empty.log; echo "FAIL: a constrained glob matching nothing was accepted"; exit 1
fi
grep -q "src/nowhere" empty.log || { cat empty.log; echo "FAIL: refusal does not name the empty glob"; exit 1; }
echo "PASS: a constrained glob matching nothing is refused"

echo "PASS: constrained source globs narrow to the build"
