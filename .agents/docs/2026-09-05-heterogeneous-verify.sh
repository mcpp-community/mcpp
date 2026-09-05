#!/usr/bin/env bash
# Ecosystem verification for mcpp 2026.9.5.3 and the heterogeneous-build round
# (design: 2026-09-05-heterogeneous-build-ecosystem-design-v2.md, section 6.3,
# rows V1 and V2). Run inside a fresh xlings subos:
#
#   xlings subos verify-953 --sandbox --cmd "MCPP_VERIFY_VERSION=2026.9.5.3 bash <this file>"
#
# and once more on the host with a GPU (MCPP_VERIFY_HOST=1), where the host
# ICD path and the CUDA lane are exercised as well.
#
# Every assertion carries its own `|| fail`; the closing line is a count.
set -uo pipefail

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
SRC="${MCPP_VERIFY_SRC:-}"            # a checkout of mcpp at v$VER or later (examples)
XL="${XLINGS_BIN:-$(command -v xlings)}"

fails=0
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }

# -- A. identity and mirrors -------------------------------------------------
section "A. identity"
[ -x "$STORE" ] || fail "no released binary at $STORE"
got=$("$STORE" --version 2>&1 | head -1)
[ "$got" = "mcpp $VER" ] && ok "$got from the store path" || fail "version is '$got'"
"$STORE" self config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || fail "mcpp self config --mirror"
"$XL" config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || fail "xlings config --mirror"
ok "mirror ${MCPP_VERIFY_MIRROR:-CN} configured for mcpp and xlings"

# -- B. the plugin collection resolves from the index -------------------------
#
# A consumer that names mcpp:plugins with one feature and imports the member
# under its declared name. The SPIR-V header's first word is the proof that the
# rule ran through the engine path (constrained glob, device source list, a
# `role = "source"` action ordered before compilation).
section "B. mcpp:plugins, feature rules-spirv"
work=$(mktemp -d)
mkdir -p "$work/spv/shaders" "$work/spv/src"
cat > "$work/spv/mcpp.toml" <<'EOT'
[package]
name = "spv-probe"
version = "0.1.0"
[language]
standard = "c++23"
modules = true
import_std = true
[dependencies.mcpp]
plugins = { version = "0.1.0", features = ["rules-spirv"], host-module = true }
[xlings.workspace]
"xim:glslang" = "15.1.0"
[build]
accel = "vulkan1.2"
sources = ["src/*.cpp", { glob = "shaders/*.comp", accel = "vulkan1.2" }]
[targets.spv-probe]
kind = "bin"
main = "src/main.cpp"
EOT
cat > "$work/spv/shaders/scale.comp" <<'EOT'
#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Data { float v[]; };
void main() { v[gl_GlobalInvocationID.x] *= 2.0; }
EOT
cat > "$work/spv/build.mcpp" <<'EOT'
import std;
import mcpp;
import mcpp.rules.spirv;
int main() {
    mcpp::rerun_if_changed_glob("shaders/**/*.comp");
    mcpp::rules::spirv::options opt;
    opt.includes = { "shaders" };
    return mcpp::rules::spirv::compile(opt) ? 0 : 1;
}
EOT
cat > "$work/spv/src/main.cpp" <<'EOT'
#include <cstdint>
#include <cstdio>
#include "scale_comp.h"
int main() { std::printf("magic=%08x\n", scale_comp_spv[0]); return scale_comp_spv[0] == 0x07230203u ? 0 : 1; }
EOT
out=$(cd "$work/spv" && "$STORE" build 2>&1 && "$STORE" run 2>&1)
printf '%s\n' "$out" | grep -q '^magic=07230203' && ok "mcpp.rules.spirv from mcpp:plugins produced a SPIR-V header" \
    || fail "spirv consumer: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"

# -- C. example 10 on the CPU driver payload ------------------------------------
section "C. examples/10-vulkan-compute on xim:mesa-lavapipe"
if [ -n "$SRC" ] && [ -d "$SRC/examples/10-vulkan-compute/app" ]; then
    ex10="$work/ex10"; cp -r "$SRC/examples/10-vulkan-compute/app" "$ex10"
    out=$(cd "$ex10" && "$STORE" build 2>&1); rc=$?
    [ $rc -eq 0 ] && ok "example 10 built" || fail "example 10 build: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"
    icd=$(ls "$ex10"/.mcpp/.xlings/data/xpkgs/xim-x-mesa-lavapipe/*/share/vulkan/icd.d/*.json 2>/dev/null | head -1)
    [ -n "$icd" ] || icd=$(ls "$HOME"/.xlings/data/xpkgs/xim-x-mesa-lavapipe/*/share/vulkan/icd.d/*.json 2>/dev/null | head -1)
    [ -n "$icd" ] && ok "lavapipe manifest at $icd" || fail "no lavapipe ICD manifest in any store"
    out=$(cd "$ex10" && VK_DRIVER_FILES="$icd" "$STORE" run 2>&1)
    printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 10 answered 12 24 36 48 on the payload driver" \
        || fail "example 10 run on lavapipe: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"
    printf '%s\n' "$out" | grep -qi 'llvmpipe' && ok "the device was llvmpipe" || printf 'note: device name not printed (%s)\n' "$(printf '%s' "$out" | head -2 | tr '\n' ' ')"
    out=$(cd "$ex10" && "$STORE" build --no-accel 2>&1 && "$STORE" run --no-accel 2>&1)
    printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 10 --no-accel answered 12 24 36 48" \
        || fail "example 10 --no-accel: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    hs=$(ls "$ex10"/.mcpp/.xlings/data/xpkgs/compat-x-vulkan-runtime/*/mcpp_generated/vulkan_runtime/HOST-SURFACE.txt 2>/dev/null | head -1)
    if [ -n "$hs" ]; then
        printf -- '--- %s\n' "$hs"; sed -n '/^## farmed/,$p' "$hs" | head -60
        hostlines=$(grep -c -- '-- host;' "$hs" || true)
        if [ -n "${MCPP_VERIFY_HOST:-}" ]; then ok "host surface recorded ($hostlines host entries, see above)"; else
            [ "$hostlines" -eq 0 ] && ok "no host library is on the runtime path in the sandbox" || fail "$hostlines host entries in a sandbox"; fi
    else fail "no HOST-SURFACE.txt written by compat.vulkan-runtime"; fi
else
    printf 'skip: MCPP_VERIFY_SRC not set or has no examples/10-vulkan-compute\n'
fi

# -- D. example 09, the CPU variant everywhere and the device on a host with one --
section "D. examples/09-cuda-kernel"
if [ -n "$SRC" ] && [ -d "$SRC/examples/09-cuda-kernel/app" ] && [ -n "${MCPP_VERIFY_CUDA:-}" ]; then
    ex09="$work/ex09"; cp -r "$SRC/examples/09-cuda-kernel/app" "$ex09"
    out=$(cd "$ex09" && "$STORE" build --no-accel 2>&1 && "$STORE" run --no-accel 2>&1)
    printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 09 --no-accel answered 12 24 36 48" \
        || fail "example 09 --no-accel: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    out=$(cd "$ex09" && "$STORE" build 2>&1); rc=$?
    [ $rc -eq 0 ] && ok "example 09 device variant compiled through mcpp.rules.cuda" || fail "example 09 build: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"
    if [ -n "${MCPP_VERIFY_HOST:-}" ]; then
        out=$(cd "$ex09" && "$STORE" run 2>&1)
        printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 09 answered 12 24 36 48 on the device" || fail "example 09 device run: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    fi
else
    printf 'skip: set MCPP_VERIFY_CUDA=1 (and MCPP_VERIFY_SRC) to exercise the CUDA lane\n'
fi

# -- E. OpenCL: the loader, the adapter, and the CPU implementation ---------------
section "E. OpenCL"
mkdir -p "$work/ocl/tests"
cat > "$work/ocl/mcpp.toml" <<'EOT'
[package]
name = "ocl-probe"
version = "0.1.0"
[dependencies.compat]
opencl = "2026.05.29"
EOT
cat > "$work/ocl/tests/platforms.cpp" <<'EOT'
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <CL/cl_ext.h>
#include <cstdio>
int main() {
    cl_uint n = 0; const cl_int rc = clGetPlatformIDs(0, nullptr, &n);
    if (rc == CL_PLATFORM_NOT_FOUND_KHR || (rc == CL_SUCCESS && n == 0)) { std::printf("platforms=0\n"); return 0; }
    if (rc != CL_SUCCESS) { std::printf("clGetPlatformIDs=%d\n", (int)rc); return 1; }
    cl_platform_id ids[8]; clGetPlatformIDs(n < 8 ? n : 8, ids, nullptr);
    std::printf("platforms=%u\n", n);
    for (cl_uint i = 0; i < n && i < 8; ++i) { char name[128] = {}; clGetPlatformInfo(ids[i], CL_PLATFORM_NAME, sizeof name, name, nullptr); std::printf("platform: %s\n", name); }
    return 0;
}
EOT
out=$(cd "$work/ocl" && "$STORE" test 2>&1)
printf '%s\n' "$out" | grep -q '^platforms=' && ok "the loader linked and enumerated: $(printf '%s' "$out" | grep '^platform' | tr '\n' ' ')" \
    || fail "opencl probe: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"
if "$XL" install pocl -y >/dev/null 2>&1; then
    ok "xim:pocl installed"
    lib=$(ls "$HOME"/.xlings/data/xpkgs/xim-x-pocl/*/lib/libpocl.so 2>/dev/null | head -1)
    out2=$(cd "$work/ocl" && OCL_ICD_FILENAMES="$lib" "$STORE" test 2>&1)
    printf '%s\n' "$out2" | grep -q 'Portable Computing Language' && ok "the pocl platform is enumerated through OCL_ICD_FILENAMES" \
        || fail "pocl not enumerated: $(printf '%s' "$out2" | grep '^platform' | tr '\n' ' ')"
else
    printf 'note: xim:pocl not installable here (not published yet, or this arch)\n'
fi

printf '\n== result: %d assertion(s) failed ==\n' "$fails"
[ "$fails" -eq 0 ]
