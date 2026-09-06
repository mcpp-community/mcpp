#!/usr/bin/env bash
# Ecosystem verification for round 5: mcpp 2026.9.6.2, compat:spirv-headers,
# and ggml-org:llamacpp's backend-vulkan feature.
#
#   xlings subos verify-962 --sandbox --cmd \
#     "MCPP_VERIFY_VERSION=2026.9.6.2 bash <this file>"
#
# and once on the host (MCPP_VERIFY_HOST=1), where a real device answers.
#
# WHAT A SANDBOX CAN AND CANNOT DECIDE HERE, AND WHY THE SPLIT IS DIFFERENT
# FROM ROUND 4's.
#
# Round 4 split on /dev: the sandbox has fourteen entries and none is a GPU, so
# any program reaching for one reports "no device" whatever the build did.
# Round 5 has a software device -- Mesa's lavapipe, a payload -- so the sandbox
# CAN run a kernel. What it cannot do is run llama.cpp on it without being told
# to: ggml keeps only Vulkan devices whose type is not `eCpu`, and lavapipe
# reports exactly that type while advertising every feature the backend
# requires. Upstream's own selector (GGML_VK_VISIBLE_DEVICES) names a device by
# index, and using it is what makes the sandbox leg meaningful rather than a
# test of the runner's hardware.
#
# EVERY CRITERION HERE NAMES THE OBJECT IT SELECTED. Round 4 produced four
# defects of one shape -- a check that chose its own object and did not say
# which -- so each section prints the path, version or device it read.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
SRC="${MCPP_VERIFY_SRC:-}"              # a checkout of llama.cpp-m at b10069.1+
XL="${XLINGS_BIN:-$(command -v xlings)}"
MODEL="${LLAMACPP_TEST_MODEL:-}"

fails=0
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# -- A. identity and mirror --------------------------------------------------
section "A. identity"
if [ ! -x "$STORE" ]; then
    fail "no released binary at $STORE"
else
    got=$("$STORE" --version 2>&1 | head -1)
    if [ "$got" = "mcpp $VER" ]; then ok "$got from $STORE"
    else fail "version is '$got' at $STORE"; fi
fi

# The SETTING, not the tool's banner: `xlings config` renders through a ui
# layer that prints nothing into a pipe, so a command substitution reads empty
# and reports a mirror that is in fact set.
"$STORE" self config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
"$XL" config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
xm=$(python3 -c "import json,os;print(json.load(open(os.path.expanduser('~/.xlings/.xlings.json'))).get('mirror',''))" 2>/dev/null)
if [ "$xm" = "${MCPP_VERIFY_MIRROR:-CN}" ]; then ok "xlings mirror is $xm"
else fail "xlings mirror is '$xm'"; fi

"$STORE" index update >/dev/null 2>&1 || true
snap=$(cat "$HOME/.mcpp/registry/data/mcpplibs/.xlings-index-version" 2>/dev/null || echo unknown)
ok "index snapshot $snap"

# -- B. compat:spirv-headers -------------------------------------------------
section "B. compat:spirv-headers"
# The criterion reads a DEFINITION back, not an exit status. A package that
# resolved and exposed the wrong include root compiles nothing, so "it built"
# would pass on an empty tree.
mkdir -p "$work/spirv/tests"
cat > "$work/spirv/mcpp.toml" <<'TOML'
[package]
name = "spirvprobe"
version = "0.1.0"
[dependencies.compat]
spirv-headers = "1.4.357.0"
TOML
cat > "$work/spirv/tests/probe.cpp" <<'CPP'
#include <spirv/unified1/spirv.hpp>
import std;
int main() {
    std::println("magic={:#x} opcap={} glcompute={}",
                 (unsigned)spv::MagicNumber,
                 (unsigned)spv::OpCapability,
                 (unsigned)spv::ExecutionModelGLCompute);
    return spv::MagicNumber == 0x07230203u ? 0 : 1;
}
CPP
out=$(cd "$work/spirv" && "$STORE" test 2>&1)
if printf '%s' "$out" | grep -q 'magic=0x7230203 opcap=17 glcompute=5'; then
    ok "compat:spirv-headers delivers the Khronos layout and definitions"
else
    fail "compat:spirv-headers probe did not read the definitions back"
    printf '%s\n' "$out" | tail -12
fi

# -- C. the feature is additive ----------------------------------------------
section "C. a CPU consumer acquires nothing of the Vulkan backend"
# The criterion is the RESOLUTION, not "the CPU build still works". A manifest
# that made every consumer download a shader compiler would also still work.
if [ -z "$SRC" ] || [ ! -f "$SRC/mcpp.toml" ]; then
    fail "set MCPP_VERIFY_SRC to a llama.cpp-m checkout"
else
    cpu="$work/cpu"; cp -r "$SRC" "$cpu"; rm -rf "$cpu/target"
    (cd "$cpu" && "$STORE" build >/dev/null 2>&1)
    res=$(ls "$cpu"/target/*/*/resolution.json 2>/dev/null | head -1)
    if [ -z "$res" ]; then
        fail "no resolution.json under $cpu/target"
    else
        ok "reading $res"
        if grep -qE '"(compat[.:]vulkan|compat[.:]spirv-headers|shaderc|mesa-lavapipe)' "$res"; then
            fail "the CPU build's resolution names a Vulkan-backend package"
            grep -oE '"(compat[.:][a-z-]+|xim:[a-z-]+)"' "$res" | sort -u | head -20
        else
            ok "no Vulkan-backend package appears in the CPU resolution"
        fi
    fi
fi

# -- D. the Vulkan backend builds, and the shaders are edges -----------------
section "D. backend-vulkan"
if [ -z "$SRC" ] || [ ! -f "$SRC/mcpp.toml" ]; then
    fail "set MCPP_VERIFY_SRC to a llama.cpp-m checkout"
else
    vk="$work/vk"; cp -r "$SRC" "$vk"; rm -rf "$vk/target"
    if (cd "$vk" && "$STORE" build --features backend-vulkan >"$work/vk.log" 2>&1); then
        ok "backend-vulkan builds"
    else
        fail "backend-vulkan build failed"
        tail -20 "$work/vk.log"
    fi

    # The shaders are GRAPH EDGES, and the count is the criterion. A build
    # program that generated them itself would leave the same artifacts here
    # and no edges in build.ninja.
    ninja=$(ls "$vk"/target/*/*/build.ninja 2>/dev/null | head -1)
    comps=$(ls "$vk"/third_party/llama.cpp/ggml/src/ggml-vulkan/vulkan-shaders/*.comp 2>/dev/null | wc -l)
    if [ -n "$ninja" ] && [ "$comps" -gt 100 ]; then
        ok "reading $ninja against $comps vendored shaders"
        rules=$(grep -c '^rule mcpp_action_' "$ninja" || true)
        if [ "$rules" -ge "$((comps + 2))" ]; then
            ok "$rules declared action rules for $comps shaders plus the generator and header"
        else
            fail "$rules action rules for $comps shaders; expected at least $((comps + 2))"
        fi
        objs=$(grep -c '\.comp\.o :' "$ninja" || true)
        if [ "$objs" -eq "$comps" ]; then
            ok "$objs generated shader sources joined the compile set"
        else
            fail "$objs generated shader objects for $comps shaders"
        fi
    else
        fail "no build.ninja or no vendored shaders under $vk"
    fi

    # The archive must CARRY them. A static library that linked nothing still
    # builds, which is how the engine defect this round fixed stayed quiet.
    ar_bin=$(command -v ar || true)
    lib=$(ls "$vk"/target/*/*/bin/libllama.a 2>/dev/null | head -1)
    if [ -n "$ar_bin" ] && [ -n "$lib" ]; then
        members=$("$ar_bin" t "$lib" | grep -c '\.comp\.o$' || true)
        if [ "$members" -eq "$comps" ]; then
            ok "$lib carries $members shader objects"
        else
            fail "$lib carries $members shader objects, expected $comps"
        fi
    else
        fail "no libllama.a or no ar to read it"
    fi
fi

# -- E. the device answers ---------------------------------------------------
section "E. the device decode equals the host decode"
if [ -z "$MODEL" ] || [ ! -f "$MODEL" ]; then
    fail "set LLAMACPP_TEST_MODEL to a .gguf (tests/support/fetch_model.py writes one)"
elif [ -z "$SRC" ]; then
    fail "set MCPP_VERIFY_SRC to a llama.cpp-m checkout"
else
    icd=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-mesa-lavapipe" \
              "$HOME/.xlings/data/xpkgs/xim-x-mesa-lavapipe" \
              -name 'lvp_icd.*.json' -print -quit 2>/dev/null || true)
    if [ -n "$icd" ]; then
        ok "software device ICD at $icd"
    else
        ok "no lavapipe payload found; using whatever driver this machine has"
    fi
    # GGML_VK_VISIBLE_DEVICES is the selector, not a workaround: ggml drops
    # `eCpu` devices by type, and a software implementation is one.
    env_prefix=(env "LLAMACPP_TEST_MODEL=$MODEL" "GGML_VK_VISIBLE_DEVICES=0")
    [ -n "$icd" ] && env_prefix+=("VK_DRIVER_FILES=$icd")
    if (cd "$work/vk" 2>/dev/null && "${env_prefix[@]}" \
            "$STORE" test vulkan_decode --features backend-vulkan >"$work/dec.log" 2>&1); then
        :
    fi
    if grep -q 'LLAMACPP_VULKAN_TEST=PASS' "$work/dec.log" 2>/dev/null; then
        ok "$(grep -m1 'vulkan device:' "$work/dec.log")"
        ok "$(grep -m1 'host token:' "$work/dec.log")"
        ok "$(grep -m1 -E 'offloaded [1-9][0-9]*/[1-9][0-9]* layers to GPU' "$work/dec.log")"
    else
        fail "the device decode did not pass"
        tail -20 "$work/dec.log" 2>/dev/null
    fi
fi

printf '\n== summary ==\n'
if [ "$fails" -eq 0 ]; then
    printf 'PASS: 0 assertions failed\n'
else
    printf 'FAIL: %s assertion(s) failed\n' "$fails"
fi
exit "$fails"
