#!/usr/bin/env bash
# requires: elf
# The accelerator dimension decides which prebuilt artifact a build may take.
#
# Two packages, because the two halves of the rule need different shapes:
#
#   gpuonly  publishes ONE artifact, for sm_90f. A build targeting sm_86 has
#            nothing it can use, so it must be refused — and the refusal must
#            name the dimension, not a digest or a triple.
#   gpukit   publishes a CPU-only artifact FIRST and a device one after it.
#            The same sm_86 build must succeed by taking the first, which is
#            the ordering that also keeps an mcpp predating this field working.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TRIPLE="x86_64-linux-gnu"

make_pkg() {                     # $1 name, $2... artifact tables
    local name="$1"; shift
    mkdir -p "$name/lib" "$name/include"
    : > "$name/lib/lib${name}.a"
    {
        echo '[package]'
        echo "name    = \"$name\""
        echo 'version = "0.1.0"'
        echo '[language]'
        echo 'standard = "c++23"'
        printf '%s\n' "$@"
    } > "$name/mcpp.toml"
}

# One device-only artifact.
make_pkg gpuonly \
'[[runtime.artifacts]]' \
'role       = "static-library"' \
'path       = "lib/libgpuonly.a"' \
'provenance = "mcpp-pack/1"' \
"abi        = \"$TRIPLE\"" \
'accel      = "cuda12.8+{sm_90f}"'

# CPU-only first, device second.
make_pkg gpukit \
'[[runtime.artifacts]]' \
'role       = "static-library"' \
'path       = "lib/libgpukit.a"' \
'provenance = "mcpp-pack/1"' \
"abi        = \"$TRIPLE\"" \
'' \
'[[runtime.artifacts]]' \
'role       = "static-library"' \
'path       = "lib/libgpukit.a"' \
'provenance = "mcpp-pack/1"' \
"abi        = \"$TRIPLE\"" \
'accel      = "cuda12.8+{sm_90f}"'

consumer() {                     # $1 dep name
    rm -rf app; "$MCPP" new app > /dev/null; cd app
    cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"
[language]
standard = "c++23"
[dependencies]
$1 = { path = "../$1" }
EOF
    cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF
    rm -f src/*.cppm
    cd ..
}

# ── 1. a device-only package refuses a build targeting another architecture ──
consumer gpuonly
cd app
if "$MCPP" build --accel 'cuda12.8+{sm_86}' > out.log 2>&1; then
    cat out.log; echo "FAIL: sm_86 accepted an sm_90f-only artifact"; exit 1
fi
grep -q 'accel' out.log || { cat out.log; echo "FAIL: refusal does not name the accel dimension"; exit 1; }
grep -q 'sm_90f' out.log || { cat out.log; echo "FAIL: refusal does not say what the artifact has"; exit 1; }
grep -q 'sm_86'  out.log || { cat out.log; echo "FAIL: refusal does not say what this build asked for"; exit 1; }
# The remedy has to be the one that can actually work. Pinning [toolchain]
# cannot change which GPU architecture a build targets, so the generic advice
# is worse than none here.
grep -q -- '--no-accel' out.log \
    || { cat out.log; echo "FAIL: refusal offers no remedy for the device axis"; exit 1; }
grep -q 'accel=cuda12.8+{sm_90f}' out.log \
    || { cat out.log; echo "FAIL: the published listing hides the device dimension"; exit 1; }
cd ..

# ── 2. the same package accepts the architecture it was built for ────────────
cd app
"$MCPP" build --accel 'cuda12.8+{sm_90}' > ok.log 2>&1 \
    || { cat ok.log; echo "FAIL: sm_90 refused by an sm_90f artifact"; exit 1; }
cd ..

# ── 3. a build asking for no accelerator is satisfied vacuously ──────────────
cd app
"$MCPP" build --no-accel > none.log 2>&1 \
    || { cat none.log; echo "FAIL: --no-accel refused"; exit 1; }
cd ..

# ── 4. CPU-first ordering lets an unmatched device build fall back ───────────
consumer gpukit
cd app
"$MCPP" build --accel 'cuda12.8+{sm_86}' > fallback.log 2>&1 \
    || { cat fallback.log; echo "FAIL: CPU-only artifact listed first did not accept sm_86"; exit 1; }
cd ..

echo "PASS: accel variant selection"
