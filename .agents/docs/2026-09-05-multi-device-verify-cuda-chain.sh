#!/usr/bin/env bash
# The whole chain, inside the sandbox, from the published indexes only:
#   xim payloads (toolkit) + mcpp-index adapters (driver) + the mcpp-index rule
#   package + the released engine.
#
# ⚠️ THE DEVICE HALF CANNOT RUN HERE, and that is a property of the sandbox
# rather than of the build. `xlings subos --sandbox` presents a minimal /dev
# with 14 entries and no `/dev/nvidia*`, so `cudaMalloc` reports "no
# CUDA-capable device is detected" no matter what was built. What the sandbox
# CAN decide is everything up to that point — that the rule package comes from
# the index, that the device unit compiles and links, and that the artifact
# reports the absence rather than crashing on it — plus the CPU variant end to
# end. The device result is asserted on the host, where the device is.
set -uo pipefail
S=/home/speak/.xlings/data/xpkgs/xim-x-mcpp/2026.9.5.2/bin/mcpp
fails=0
fail() { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails+1)); }

cd /home/speak/cuda-app || { echo "no project"; exit 1; }
rm -rf target .mcpp mcpp.lock

out=$("$S" run 2>&1)
printf '%s\n' "$out" | grep -q "mcpplibs.rules-cuda" \
    || fail "the rule package did not come from the index ($(printf '%s' "$out" | tail -3 | tr '\n' ' '))"
printf '%s\n' "$out" | grep -q "Finished" \
    || fail "the device build did not complete ($(printf '%s' "$out" | tail -4 | tr '\n' ' '))"
printf '%s\n' "$out" | grep -q "no CUDA-capable device is detected" \
    || fail "the artifact did not report the sandbox's missing device cleanly ($(printf '%s' "$out" | tail -3 | tr '\n' ' '))"
printf '%s\n' "$out" | grep -q "mcpplibs.rules-cuda" \
    && printf '%s\n' "$out" | grep -q "Finished" \
    && echo "ok: the rule package came from the index and the device build completed"
printf '%s\n' "$out" | grep -q "no CUDA-capable device is detected" \
    && echo "ok: with no device node in the sandbox, the artifact says so and does not crash"

out2=$("$S" run --no-accel 2>&1)
printf '%s\n' "$out2" | grep -q "12 24 36 48" \
    || fail "the CPU variant is wrong ($(printf '%s' "$out2" | tail -4 | tr '\n' ' '))"
printf '%s\n' "$out2" | grep -q "12 24 36 48" \
    && echo "ok: --no-accel produces the right answer from the host CPU"

[ "$fails" -eq 0 ] && { echo "CUDA CHAIN OK (device half deferred to the host)"; exit 0; }
echo "$fails ASSERTION(S) FAILED"; exit 1
