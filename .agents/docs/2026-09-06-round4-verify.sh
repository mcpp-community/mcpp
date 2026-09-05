#!/usr/bin/env bash
# Ecosystem verification for mcpp 2026.9.6.1 and round 4 of the
# heterogeneous-build work (design:
# 2026-09-05-heterogeneous-build-ecosystem-design-v2.md, section 7.3, rows V3
# and V4). Run inside a fresh xlings subos:
#
#   xlings subos verify-961 --sandbox --cmd "MCPP_VERIFY_VERSION=2026.9.6.1 bash <this file>"
#
# and once more on the host with a GPU (MCPP_VERIFY_HOST=1), where the device
# half of each lane is exercised as well.
#
# WHAT A SANDBOX CANNOT DECIDE, AND WHY THE SCRIPT IS SPLIT.
#
# `xlings subos --sandbox` has 14 entries in /dev and none of them is
# /dev/nvidia*, so any program that reaches for a GPU reports "no CUDA-capable
# device" whatever the build did. The sandbox therefore asserts that the LINK
# is complete -- the payload resolved, the rule ran, the object was produced,
# the artifact's runtime closure is satisfied -- and the device result is
# asserted on the host. A criterion that named the device result in the sandbox
# would be unreachable by construction rather than a statement about the
# ecosystem.
#
# Every assertion carries its own `|| fail`; the closing line is a count.
set -uo pipefail

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
SRC="${MCPP_VERIFY_SRC:-}"            # a checkout of mcpp at v$VER or later (examples)
XL="${XLINGS_BIN:-$(command -v xlings)}"
PLUGINS_VERSION="${MCPP_VERIFY_PLUGINS:-0.2.0}"

fails=0
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }

# -- A. identity and mirrors -------------------------------------------------
section "A. identity"
[ -x "$STORE" ] || fail "no released binary at $STORE"
got=$("$STORE" --version 2>&1 | head -1)
[ "$got" = "mcpp $VER" ] && ok "$got from the store path" || fail "version is '$got'"

# THE SETTING, NOT THE TOOL'S OUTPUT. `xlings config` renders a banner through
# its ui layer, which prints nothing when its stdout is a pipe -- so a command
# substitution reads an empty string and reports a mirror that is in fact set.
# The file the tool writes answers the same question and cannot be suppressed.
"$STORE" self config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
"$XL" config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
xm=$(python3 -c "import json,os;print(json.load(open(os.path.expanduser('~/.xlings/.xlings.json'))).get('mirror',''))" 2>/dev/null)
[ "$xm" = "${MCPP_VERIFY_MIRROR:-CN}" ] && ok "xlings mirror is $xm" || fail "xlings mirror is '$xm', not ${MCPP_VERIFY_MIRROR:-CN}"

# The registry's index snapshot is shared with whatever wrote it last, and the
# refresh is resolution-driven with a freshness window -- so a package merged
# minutes ago resolves as "download artifact missing" until the snapshot moves.
"$STORE" index update >/dev/null 2>&1 || true
snap=$(cat "$HOME/.mcpp/registry/data/mcpplibs/.xlings-index-version" 2>/dev/null || echo "unknown")
ok "index snapshot $snap"

work=$(mktemp -d)

# -- B. the two new payloads install and answer ------------------------------
section "B. xim:shaderc and xim:hip-nvidia"

# glslc: the criterion is a SPIR-V module's magic number, not an exit status --
# a compiler that wrote an empty file would also exit 0.
"$XL" install shaderc -y >/dev/null 2>&1 || true
glslc=$(ls "$HOME"/.xlings/data/xpkgs/*-x-shaderc/*/bin/glslc "$HOME"/.mcpp/registry/data/xpkgs/*-x-shaderc/*/bin/glslc 2>/dev/null | head -1)
if [ -n "$glslc" ]; then
    ok "xim:shaderc installed at $glslc"
    printf '#version 450\nlayout(local_size_x=64) in;\nlayout(std430,binding=0) buffer B { uint v[]; };\nvoid main(){ v[gl_GlobalInvocationID.x] *= 2; }\n' > "$work/s.comp"
    if "$glslc" --target-env=vulkan1.2 -fshader-stage=comp -O -o "$work/s.spv" "$work/s.comp" >"$work/glslc.log" 2>&1; then
        magic=$(python3 -c "import struct,sys;d=open('$work/s.spv','rb').read();print('%08x'%struct.unpack('<I',d[:4])[0] if len(d)>=4 else 'short')")
        [ "$magic" = "07230203" ] && ok "glslc produced a SPIR-V module (magic=$magic)" || fail "glslc output magic is $magic"
    else
        fail "glslc failed: $(tail -2 "$work/glslc.log" | tr '\n' ' ')"
    fi
    # Every DT_NEEDED inside a payload. This is the property that makes the
    # repack a payload rather than a copy of somebody's /usr.
    # THE LOADER IS NOT A DEPENDENCY RESOLVED FROM THE HOST. glibc's `ldd`
    # prints the program interpreter with `=>` like everything else, so a
    # filter that only looks for that arrow counts `/lib64/ld-linux-x86-64.so.2`
    # as a host library and fails a payload that is in fact complete. The
    # interpreter is chosen by the ELF header, not searched for, and which one
    # a payload uses is the subject of its own recipe rather than of this
    # check.
    outside=$(ldd "$glslc" 2>/dev/null | awk '/=>/ {print $3}' | grep -v '^$' \
              | grep -v "$HOME" | grep -v '^(' | grep -v 'ld-linux' | head -3)
    [ -z "$outside" ] && ok "every glslc dependency resolves inside a payload" || fail "glslc resolves outside the store: $(echo "$outside" | tr '\n' ' ')"
else
    fail "xim:shaderc left no bin/glslc in either store"
fi

# hip-nvidia: headers only, and the dispatch header is what makes the NVIDIA
# platform reachable at all.
"$XL" install hip-nvidia -y >/dev/null 2>&1 || true
hipinc=$(ls -d "$HOME"/.xlings/data/xpkgs/*-x-hip-nvidia/*/include "$HOME"/.mcpp/registry/data/xpkgs/*-x-hip-nvidia/*/include 2>/dev/null | head -1)
if [ -n "$hipinc" ]; then
    ok "xim:hip-nvidia installed at $hipinc"
    for h in hip/hip_runtime.h hip/hip_version.h hip/nvidia_detail/nvidia_hip_runtime.h hip/amd_detail/amd_hip_runtime_pt_api.h; do
        [ -f "$hipinc/$h" ] && ok "  $h" || fail "hip-nvidia is missing $h"
    done
    elf=$(find "$(dirname "$hipinc")" -type f -exec sh -c 'head -c4 "$1" | grep -q ELF' _ {} \; -print 2>/dev/null | head -1)
    [ -z "$elf" ] && ok "hip-nvidia carries no binaries, which is what a header layer means" || fail "hip-nvidia contains an ELF file: $elf"
else
    fail "xim:hip-nvidia left no include/ in either store"
fi

# -- C. the dpcpp payload's programs start, and report ------------------------
#
# Five of them shipped with no search path at all and could not start once
# installed; the measurement that put a device in this recipe's comment had
# been taken with LD_LIBRARY_PATH set. The assertion here is that they start
# and that no adapter fails on a library inside this same payload. WHICH
# devices are listed is the machine's answer and is deliberately not asserted.
section "C. xim:dpcpp"
"$XL" install dpcpp -y >/dev/null 2>&1 || true
dp=$(ls -d "$HOME"/.xlings/data/xpkgs/*-x-dpcpp/*/ "$HOME"/.mcpp/registry/data/xpkgs/*-x-dpcpp/*/ 2>/dev/null | head -1)
if [ -n "$dp" ]; then
    ok "xim:dpcpp installed at $dp"
    for p in sycl-ls sycl-prof sycl-trace sycl-sanitize syclbin-dump; do
        [ -f "$dp/bin/$p" ] || continue
        err=$("$dp/bin/$p" --help 2>&1 >/dev/null | head -1)
        case "$err" in
            *"error while loading shared libraries"*) fail "bin/$p cannot start: $err" ;;
            *) ok "  bin/$p starts" ;;
        esac
        # DT_RPATH, not DT_RUNPATH, and the difference decides the adapters:
        # RUNPATH is honoured for the program's own DT_NEEDED and not for a
        # dlopen beneath it, so a RUNPATH here lets every one of these start
        # and report no devices at all.
        tag=$(readelf -d "$dp/bin/$p" 2>/dev/null | grep -oE 'RPATH|RUNPATH' | head -1)
        [ "$tag" = "RPATH" ] && ok "  bin/$p carries DT_RPATH" || fail "bin/$p carries '$tag', not RPATH"
    done
    # And the LIBRARIES must have none: a RUNPATH on one of them switches off
    # the inherited RPATH of whatever loaded it, which is what cut a consumer's
    # artifact off from its own driver farm.
    adapter=$(ls "$dp"/lib/libur_adapter_*.so.0.* 2>/dev/null | head -1)
    if [ -n "$adapter" ]; then
        n=$(readelf -d "$adapter" 2>/dev/null | grep -cE 'RPATH|RUNPATH')
        [ "$n" -eq 0 ] && ok "  the adapters carry no search path of their own, so they inherit the caller's" \
            || fail "$(basename "$adapter") carries a search path; it would cut its loader off from the artifact's farm"
    fi
    # ONE DEFECT, ONE REPORT, AT ITS CAUSE. A program that could not start
    # leaves `error while loading shared libraries: libsycl.so.9` on the same
    # stderr the adapter check reads, so running both turns one failure into
    # three and the third names the wrong thing.
    lsout=$("$dp/bin/sycl-ls" --verbose 2>&1)
    if printf '%s' "$lsout" | grep -q 'error while loading shared libraries'; then
        printf 'note: adapter checks skipped -- sycl-ls itself could not start (reported above)\n'
    else
        for own in libsycl.so libumf.so libur_loader.so; do
            printf '%s' "$lsout" | grep -q "$own.*cannot open" && fail "an adapter could not find $own, which is in this payload" || ok "  no adapter failed on $own"
        done
    fi
    if [ -n "${MCPP_VERIFY_HOST:-}" ]; then
        printf '%s' "$lsout" | grep -q 'cuda:gpu' && ok "sycl-ls enumerates a CUDA device on this host" || fail "sycl-ls found no CUDA device on a host that has one"
    else
        ok "device enumeration not asserted in a sandbox (no /dev/nvidia*)"
    fi
else
    fail "xim:dpcpp is not installed in either store"
fi

# -- D. the collection, from the index, with the new features -----------------
section "D. mcpp:plugins $PLUGINS_VERSION"
mkdir -p "$work/plug/src"
cat > "$work/plug/mcpp.toml" <<EOT
[package]
name = "plug-probe"
version = "0.1.0"
[language]
standard = "c++23"
modules = true
import_std = true
[dependencies.mcpp]
plugins = { version = "$PLUGINS_VERSION", features = ["rules-hip", "rules-sycl", "rules-spirv"], host-module = true }
[targets.plug-probe]
kind = "bin"
main = "src/main.cpp"
EOT
cat > "$work/plug/src/main.cpp" <<'EOT'
int main() { return 0; }
EOT
# The build program imports all three members by their declared module names.
# A feature that did not put its unit into the source set fails HERE, as an
# unknown module, which is the assertion that the feature table is live.
cat > "$work/plug/build.mcpp" <<'EOT'
import std;
import mcpp;
import mcpp.rules.hip;
import mcpp.rules.sycl;
import mcpp.rules.spirv;
int main() { std::println("three members imported"); return 0; }
EOT
out=$(cd "$work/plug" && "$STORE" build 2>&1)
printf '%s\n' "$out" | grep -qi 'error' && fail "importing the three members failed: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')" \
    || ok "mcpp.rules.hip, .sycl and .spirv all import from mcpp:plugins $PLUGINS_VERSION"

# -- E. the HIP lane ----------------------------------------------------------
section "E. examples/12-hip-kernel"
if [ -n "$SRC" ] && [ -d "$SRC/examples/12-hip-kernel/app" ]; then
    ex="$work/ex12"; cp -r "$SRC/examples/12-hip-kernel/app" "$ex"
    out=$(cd "$ex" && "$STORE" build --no-accel 2>&1 && "$STORE" run --no-accel 2>&1)
    printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 12 --no-accel answered 12 24 36 48" \
        || fail "example 12 --no-accel: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    out=$(cd "$ex" && "$STORE" build -v 2>&1); rc=$?
    if [ $rc -eq 0 ]; then
        ok "example 12 device variant compiled through mcpp.rules.hip"
        # THE HOST-LEAK ASSERTION. Both device rules compiled successfully on a
        # developer machine while reading /usr/include, and the only place that
        # was visible was the command line.
        printf '%s' "$out" | grep -q '/usr/include\|/usr/lib' && fail "a host path reached the HIP command line" \
            || ok "  no /usr path on any command line"
    else
        fail "example 12 build: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"
    fi
    if [ -n "${MCPP_VERIFY_HOST:-}" ]; then
        out=$(cd "$ex" && "$STORE" run 2>&1)
        printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 12 answered 12 24 36 48 on the device" \
            || fail "example 12 device run: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    else
        ok "device result not asserted in a sandbox"
    fi
else
    printf 'skip: set MCPP_VERIFY_SRC to a checkout carrying examples/12-hip-kernel\n'
fi

# -- F. the SYCL lane ---------------------------------------------------------
section "F. examples/11-sycl-kernel"
if [ -n "$SRC" ] && [ -d "$SRC/examples/11-sycl-kernel/app" ]; then
    ex="$work/ex11"; cp -r "$SRC/examples/11-sycl-kernel/app" "$ex"; ex11_built="$ex"
    out=$(cd "$ex" && "$STORE" build --no-accel 2>&1 && "$STORE" run --no-accel 2>&1)
    printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 11 --no-accel answered 12 24 36 48" \
        || fail "example 11 --no-accel: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    out=$(cd "$ex" && "$STORE" build -v 2>&1); rc=$?
    if [ $rc -eq 0 ]; then
        ok "example 11 device variant compiled through mcpp.rules.sycl"
        printf '%s' "$out" | grep -q '/usr/include\|/usr/lib' && fail "a host path reached the SYCL command line" \
            || ok "  no /usr path on any command line"
        # BOTH objects. The unit's own carries the device image; the
        # device-link wrapper is what registers it, and without the second the
        # program links and finds no kernel at run time.
        [ -f "$ex/target/.build-mcpp/out/sycl_device_link.o" ] && ok "  the device-link wrapper was produced" \
            || fail "no sycl_device_link.o: the device image is in the object and registered by nothing"
    else
        fail "example 11 build: $(printf '%s' "$out" | tail -4 | tr '\n' ' ')"
    fi
    if [ -n "${MCPP_VERIFY_HOST:-}" ]; then
        out=$(cd "$ex" && "$STORE" run 2>&1)
        printf '%s\n' "$out" | grep -q '12 24 36 48' && ok "example 11 answered 12 24 36 48 on the device" \
            || fail "example 11 device run: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    else
        ok "device result not asserted in a sandbox"
    fi
else
    printf 'skip: set MCPP_VERIFY_SRC to a checkout carrying examples/11-sycl-kernel\n'
fi

# -- G. compat.sycl-runtime: the farm, and what it had to contain -------------
#
# The adapter's whole job is that a soname the artifact reaches through the
# loader is on its runtime search path. Three of its entries were added by a
# failure rather than by design, and this section names each one so a farm that
# silently loses it is caught: `libumf.so.1` (the chain, without which the
# runtime loads and enumerates nothing), `libstdc++.so.6` (which compat.cudart
# deliberately does not farm, and which a SYCL artifact needs because it links
# libc++) and `libz.so.1` (which a developer machine happened to have).
section "G. compat.sycl-runtime 2026.09.07"
# THE FARM THE EXAMPLE RESOLVED, NOT WHATEVER THE STORE HOLDS.
#
# A store that has seen two adapter versions holds two farms, and picking one
# by sorting is picking by accident: the sandbox run of 2026-09-06 reported
# `ok` for every soname of a farm the example under test had not used, because
# a DIFFERENT version happened to be the only one in that store. The example's
# own `resolution.json` names the directory it was actually built against.
farm=""
if [ -n "${ex11_built:-}" ]; then
    farm=$(python3 - "$ex11_built" <<'PY' 2>/dev/null
import glob, json, sys
for f in glob.glob(sys.argv[1] + "/target/*/*/resolution.json"):
    for m in json.dumps(json.load(open(f))).split('"'):
        if m.endswith("sycl_runtime/lib"):
            print(m); raise SystemExit
PY
)
fi
# Only if the example was not built here: then any installed farm is the best
# available evidence, and the line says which one it read.
if [ -z "$farm" ]; then
    farm=$(ls -d "$HOME"/.mcpp/registry/data/xpkgs/compat-x-sycl-runtime/*/mcpp_generated/sycl_runtime/lib 2>/dev/null | sort -V | tail -1)
    [ -n "$farm" ] && printf 'note: no example build to read; falling back to the newest farm in the store\n'
fi
if [ -n "$farm" ]; then
    ok "farm at $farm"
    for so in libsycl.so.9 libur_loader.so.0 libumf.so.1 libstdc++.so.6 libz.so.1 libdl.so.2 libcuda.so.1; do
        [ -e "$farm/$so" ] && ok "  $so" || fail "the farm is missing $so"
    done
    # No unversioned name: mcpp puts runtime.library_dirs on the LINK line as
    # well, so a bare libsycl.so here would be found by -lsycl and bind the
    # build to the farm instead of to the payload the project declared.
    bare=$(ls "$farm" | grep -E '\.so$' | head -3)
    [ -z "$bare" ] && ok "  no unversioned soname in the farm" || fail "unversioned names in the farm: $(echo "$bare" | tr '\n' ' ')"
else
    printf 'skip: no compat.sycl-runtime farm found (build example 11 first)\n'
fi

printf '\n== result: %d assertion(s) failed ==\n' "$fails"
[ "$fails" -eq 0 ]
