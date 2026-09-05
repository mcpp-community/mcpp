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
# The setting is the criterion, not the exit code: the subos shim of a fresh
# subos has been observed to return non-zero from `config --mirror` while the
# value was written, so both tools are asked what they hold afterwards.
"$STORE" self config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
"$XL" config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
# Both tools print their configuration banner on stderr, so a `2>/dev/null`
# here reads an empty string and reports a mirror that is in fact set.
xm=$("$XL" config 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -i 'mirror' | head -1 | awk '{print $NF}')
[ "$xm" = "${MCPP_VERIFY_MIRROR:-CN}" ] && ok "xlings mirror is $xm" || fail "xlings mirror is '$xm', not ${MCPP_VERIFY_MIRROR:-CN}"
mm=$("$STORE" self config 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -i 'mirror' | head -1 | awk '{print $NF}')
[ -n "$mm" ] && ok "mcpp mirror is $mm" || printf 'note: mcpp self config does not print its mirror (%s)\n' "$("$STORE" self config 2>&1 | head -1)"

# THE REGISTRY IS SHARED WITH WHATEVER WROTE IT LAST, AND ITS INDEX SNAPSHOT
# IS PART OF THAT. A fresh subos inherits the snapshot the registry holds, and
# the refresh is resolution-driven with a freshness window, so a package merged
# minutes ago resolves as `download artifact missing` until the snapshot moves.
# Measured 2026-09-05: section E failed against a snapshot four commits behind.
"$STORE" index update >/dev/null 2>&1 || true
snap=$(cat "$HOME/.mcpp/registry/data/mcpplibs/.xlings-index-version" 2>/dev/null || echo "unknown")
ok "index snapshot $snap"

# Mesa creates its allocations through an anonymous file and falls back to
# XDG_RUNTIME_DIR when memfd is unavailable. A subos root has no /run, so the
# inherited value names a directory that does not exist and lavapipe fails with
# `Failed to create anonymous file for memory allocations` before it reports a
# device. A writable directory inside the sandbox is the whole fix.
if [ ! -d "${XDG_RUNTIME_DIR:-/nonexistent}" ]; then
    XDG_RUNTIME_DIR="$HOME/.cache/xdg-runtime"; mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"; export XDG_RUNTIME_DIR
    ok "XDG_RUNTIME_DIR redirected to $XDG_RUNTIME_DIR"
fi

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
    # mcpp provisions [xlings.workspace] payloads into its own registry
    # (~/.mcpp/registry), which is where the manifest lives; the other two
    # locations cover a project-local sandbox and a plain xlings install.
    icd=$(ls "$HOME"/.mcpp/registry/data/xpkgs/xim-x-mesa-lavapipe/*/share/vulkan/icd.d/*.json \
             "$ex10"/.mcpp/.xlings/data/xpkgs/xim-x-mesa-lavapipe/*/share/vulkan/icd.d/*.json \
             "$HOME"/.xlings/data/xpkgs/xim-x-mesa-lavapipe/*/share/vulkan/icd.d/*.json 2>/dev/null | head -1)
    if [ -z "$icd" ]; then
        # The example declares the payload (mcpp#569); an older checkout does
        # not, in which case the payload is installed here so the criterion is
        # still the driver and not the manifest's history.
        "$XL" install mesa-lavapipe -y >/dev/null 2>&1 || true
        icd=$(ls "$HOME"/.xlings/data/xpkgs/xim-x-mesa-lavapipe/*/share/vulkan/icd.d/*.json 2>/dev/null | head -1)
    fi
    [ -n "$icd" ] && ok "lavapipe manifest at $icd" || fail "no lavapipe ICD manifest in any store"
    out=$(cd "$ex10" && VK_DRIVER_FILES="$icd" "$STORE" run 2>&1)
    printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 10 answered 12 24 36 48 on the payload driver" \
        || fail "example 10 run on lavapipe: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"
    printf '%s\n' "$out" | grep -qi 'llvmpipe' && ok "the device was llvmpipe" || printf 'note: device name not printed (%s)\n' "$(printf '%s' "$out" | head -2 | tr '\n' ' ')"
    out=$(cd "$ex10" && "$STORE" build --no-accel 2>&1 && "$STORE" run --no-accel 2>&1)
    printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 10 --no-accel answered 12 24 36 48" \
        || fail "example 10 --no-accel: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    hs=$(ls "$HOME"/.mcpp/registry/data/xpkgs/compat-x-vulkan-runtime/*/mcpp_generated/vulkan_runtime/HOST-SURFACE.txt \
            "$ex10"/.mcpp/.xlings/data/xpkgs/compat-x-vulkan-runtime/*/mcpp_generated/vulkan_runtime/HOST-SURFACE.txt 2>/dev/null | head -1)
    if [ -n "$hs" ]; then
        printf -- '--- %s\n' "$hs"; sed -n '/^## farmed/,$p' "$hs" | head -60
        # WHAT A SANDBOX CAN AND CANNOT ASSERT. A subos shares the host's
        # /usr, so the farm sees the host's proprietary driver there and a
        # count of zero host entries is not reachable by construction -- the
        # earlier form of this check asserted it and failed on a correct farm.
        # What the sandbox does measure is the substitution invariant: every
        # soname compat.vulkan-runtime declares a payload for is taken from
        # that payload, and a declaration that did not take effect says so in
        # the report.
        hostlines=$(grep -c -- '-- host;' "$hs" || true)
        payloads=$(grep -c -- '-- payload;' "$hs" || true)
        undeclared=$(grep -c 'did not take effect' "$hs" || true)
        printf 'note: %s payload substitutions, %s host entries\n' "$payloads" "$hostlines"
        [ "$undeclared" -eq 0 ] && ok "every declared payload took effect" \
            || fail "$undeclared declared payload(s) did not take effect"
        [ "$payloads" -ge 20 ] && ok "$payloads farmed libraries come from payloads" \
            || fail "only $payloads payload substitutions; the declared set did not install"
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
    lib=$(ls "$HOME"/.xlings/data/xpkgs/xim-x-pocl/*/lib/libpocl.so "$HOME"/.mcpp/registry/data/xpkgs/xim-x-pocl/*/lib/libpocl.so 2>/dev/null | head -1)
    out2=$(cd "$work/ocl" && OCL_ICD_FILENAMES="$lib" "$STORE" test 2>&1)
    printf '%s\n' "$out2" | grep -q 'Portable Computing Language' && ok "the pocl platform is enumerated through OCL_ICD_FILENAMES" \
        || fail "pocl not enumerated: $(printf '%s' "$out2" | grep '^platform' | tr '\n' ' ')"
else
    printf 'note: xim:pocl not installable here (not published yet, or this arch)\n'
fi

printf '\n== result: %d assertion(s) failed ==\n' "$fails"
[ "$fails" -eq 0 ]
