#!/usr/bin/env bash
# Build every example in `examples/` that this runner can build.
#
# WHY THIS EXISTS. Until it did, no CI job built any example. e2e 616 checks
# that the curriculum and the table documenting it agree structurally, and says
# so explicitly: it builds nothing. Every example here could have stopped
# compiling and the first report would have come from a reader.
#
# THE LIST IS DERIVED FROM THE TREE. `BUILD` and `SKIP` are compared against
# the roots actually on disk, and a root in neither list fails this script.
# That is the denominator: adding an example forces a decision about whether CI
# can build it, instead of leaving it silently uncovered.
#
# A SKIP CARRIES ITS REASON AND ITS ELSEWHERE. "CI cannot build this" is only
# acceptable when something else does, and each entry says what.
set -uo pipefail

MCPP="${MCPP:?set MCPP}"
cd "$(dirname "$0")/../.."

BUILD=(
    examples/01-hello
    examples/02-with-deps
    examples/03-pack-static
    examples/04-workspace
    examples/08-build-rules/app
    # The CPU-only path of the multi-backend example: no payloads, and it is
    # where `cfg(accelerator = "none")` is exercised. The device paths are
    # opt-in via --accel and are covered by the rule packages' own CI.
    # Built here for one reason worth the cost: it is the only example whose
    # CPU-only configuration exercises `cfg(accelerator = "none")` and two rule
    # packages in one build program, and both of those are engine paths that a
    # description cannot cover. Its `[toolchain] default = "llvm@22.1.8"` means
    # this job installs an LLVM payload it otherwise would not -- the CUDA leg
    # takes the clang route, because the nvcc route on the 12.9 line is refused
    # by nvcc's own front end and the 13.x line raises the driver floor to r580.
    # The device payloads are NOT installed: they are gated on the accelerator,
    # and this builds without one.
    examples/09-heterogeneous/multi-backend
    # The island boundary with nothing on top. Its island is an ordinary C file,
    # so it needs no device and no device payload -- only `mcpp:plugins` with
    # `tools-island`. It is the one example that runs the arrangement the
    # plugins README records as measured: a consumer importing the GENERATED
    # module and linking against an implementation another compiler produced.
    examples/09-heterogeneous/boundary
    # Declares features rather than consuming them. The next step asserts the
    # criterion that matters -- that the default build's RESOLUTION does not
    # name the optional package -- which a build alone cannot show.
    examples/11-features/counters
    examples/11-features/greeter
    # A device language the engine does not know, and the compiler for it.
    #
    # `toyc` is built here as an ordinary package as well as by the app as a
    # host tool, and the two are not the same signal: this one fails at the
    # compiler, the app's fails somewhere in `tools = [...]` / `reexport` /
    # `dep_bin`, and a single line telling them apart is worth one build of a
    # three-file package.
    examples/12-a-new-device-language/toyc
    examples/12-a-new-device-language/app
    # One source, three platforms. BUILT here rather than skipped, because its
    # HOST build needs no payload at all -- the `[target.*-linux-android]`
    # sections are inert unless that target is selected, which is itself worth
    # one build: a manifest that names a target the runner has no payload for
    # must still parse and build for the host.
    #
    # The cross legs are not built here. `wasm32-emscripten` and the two
    # Android rows would pull `xim:emsdk` and `xim:android-ndk` -- about 1.5 GB
    # between them -- and the signal already exists elsewhere:
    # ci-target-matrix scans every row on four hosts, tests/e2e/641 asserts the
    # vocabulary, and the example's README records the measured artifacts and
    # the emulator run for the binary it describes.
    examples/13-platform-targets
)

# `key|reason`.
SKIP=(
    "examples/05-lib-distribution/producer|packed and then consumed by the dedicated step below, which is the order the example's own README gives"
    "examples/05-lib-distribution/consumer|same"
    "examples/06-openkal-cross|cross-builds to a second target; the payload matrix is what the target-matrix workflow already covers, and repeating it here would double a long job for no new signal"
    "examples/07-project-subos|provisions a project-local sub-OS, which e2e 27_self_contained_home covers directly and far more cheaply"
    "examples/08-build-rules/rules-embed|a rule package is not a standalone build: its interface imports the bundled mcpp module, which exists only inside a consumer's build. Verified by building 08-build-rules/app, the same way mcpp-plugins verifies its own members"
    "examples/08-build-rules/rules-tidy|same"
    "examples/12-a-new-device-language/rules-toy|a rule package is not a standalone build: its module imports the bundled mcpp module, which exists only inside a consumer's build program. Verified by building 12-a-new-device-language/app"
    "examples/09-heterogeneous/cuda/app|needs the CUDA payload set, and a device to run; the rule package is covered by mcpp-plugins' own CI"
    "examples/09-heterogeneous/hip/app|same, for the HIP payloads"
    "examples/09-heterogeneous/sycl/app|needs the dpcpp payload (over a gigabyte) and a device its runtime accepts"
    "examples/09-heterogeneous/vulkan/app|built AND RUN by the next step of this job, on the lavapipe payload, which needs no GPU"
    "examples/10-graphics/offscreen|built AND RUN by its own step of this job, on the same lavapipe payload. It renders a real graphics pipeline offscreen and asserts the pixels, which is why it runs there rather than here"
    "examples/09-heterogeneous/cann/app|its device leg needs the Ascend DRIVER, which a runner does not have: the kernel compiles and the object links, and then `libascend_hal.so` is missing -- and only that one, since 2026.9.6.6 models DT_RPATH inheritance. Correct on a machine with no NPU. Its CPU leg does build, and is not built here only because it would make this job resolve a fifth rule package for one example. Covered by the measurements in its README"
)

# Every ROOT manifest in the tree: a directory with an `mcpp.toml` that has no
# ancestor manifest below `examples/`. A workspace member is not a root.
mapfile -t FOUND < <(
    find examples -name mcpp.toml -not -path '*/target/*' | while read -r m; do
        d=$(dirname "$m"); p=$(dirname "$d"); root=1
        while [ "$p" != "." ] && [ "$p" != "examples" ]; do
            [ -f "$p/mcpp.toml" ] && { root=0; break; }
            p=$(dirname "$p")
        done
        [ "$root" = 1 ] && echo "$d"
    done | sort -u
)

fail=0
declare -A known=()
for b in "${BUILD[@]}"; do known["$b"]=build; done
for s in "${SKIP[@]}"; do known["${s%%|*}"]=skip; done

echo "== the tree has ${#FOUND[@]} example roots =="
for f in "${FOUND[@]}"; do
    if [ -z "${known[$f]:-}" ]; then
        echo "ERROR: $f is in neither BUILD nor SKIP."
        echo "       Add it to one. A skip must say why, and where it is covered instead."
        fail=1
    fi
done
for k in "${!known[@]}"; do
    printf '%s\n' "${FOUND[@]}" | grep -Fxq "$k" || {
        echo "ERROR: $k is listed here but is not a root in the tree."
        fail=1
    }
done
[ "$fail" -eq 0 ] || exit 1

for s in "${SKIP[@]}"; do
    echo "SKIP ${s%%|*}: ${s#*|}"
done

built=0
for d in "${BUILD[@]}"; do
    echo "== $d =="
    if (cd "$d" && "$MCPP" build); then built=$((built + 1)); else
        echo "FAIL: $d did not build"; fail=1
    fi
done

# ── 05-lib-distribution: pack, then consume ────────────────────────────────
# The consumer names the produced directory by its ABI TAG, which is what the
# README tells a reader to do. Building it here is therefore also a check that
# the tag in the manifest is still the tag `mcpp pack` produces -- a claim the
# example makes in prose and nothing else verified.
echo "== examples/05-lib-distribution (pack, then consume) =="
if (cd examples/05-lib-distribution/producer && "$MCPP" pack mathkit); then
    named=$(sed -n 's/.*path = "\(\.\.\/producer[^"]*\)".*/\1/p' \
            examples/05-lib-distribution/consumer/mcpp.toml | head -1)
    if [ -d "examples/05-lib-distribution/consumer/$named" ]; then
        if (cd examples/05-lib-distribution/consumer && "$MCPP" build); then
            built=$((built + 1))
        else
            echo "FAIL: the consumer did not build"; fail=1
        fi
    else
        echo "FAIL: the consumer names '$named', which mcpp pack did not produce."
        echo "      Produced:"; ls examples/05-lib-distribution/producer/target/dist 2>&1 | sed 's/^/        /'
        fail=1
    fi
else
    echo "FAIL: mcpp pack mathkit"; fail=1
fi

echo "== built $built of $(( ${#BUILD[@]} + 1 )) =="
[ "$built" -eq $(( ${#BUILD[@]} + 1 )) ] || fail=1
exit "$fail"
