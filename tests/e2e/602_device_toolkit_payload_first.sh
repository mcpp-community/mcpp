#!/usr/bin/env bash
# The device-toolkit report reads a PAYLOAD before it reads the host.
#
# A toolkit installed through xlings is the one a build will use, and it is
# also the newer one -- measured on the development machine, a payload states
# `gcc <= 15` where the distribution's CUDA 12.0 states `gcc <= 12`. Reporting
# the host's bound while the build uses the payload's answers a question nobody
# asked, and the two answers differ by three major compiler versions.
#
# No CUDA is required to assert the ordering: a fabricated `crt/host_config.h`
# in a fabricated payload directory is enough, because what is under test is
# which of two files the report reads.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# A payload store laid out the way xlings lays one out, holding a header whose
# bound is one nothing on a real machine would state.
PAYLOAD="$TMP/xlings/data/xpkgs/local-x-cuda-crt/99.9.99/include/crt"
mkdir -p "$PAYLOAD"
cat > "$PAYLOAD/host_config.h" <<'HDR'
#if __GNUC__ > 41
#error -- unsupported GNU version! gcc versions later than 41 are not supported!
#endif
HDR

out="$TMP/doctor.log"
XLINGS_HOME="$TMP/xlings" "$MCPP" self doctor > "$out" 2>&1 || true

grep -q "device toolkit" "$out" || { cat "$out"; echo "FAIL: no device toolkit section"; exit 1; }

# The bound reported must be the payload's 41, whatever the host has. On a
# machine with no CUDA at all this is also the only way the section appears.
if ! grep -qE "<= ?41|bound of 41" "$out"; then
    grep -A3 "device toolkit" "$out"
    echo "FAIL: the report did not read the payload's host_config.h"
    exit 1
fi
echo "PASS: the payload's bound is the one reported"

# The control. Without the payload store, the same command must NOT report 41 --
# otherwise the assertion above would pass against a doctor that hardcodes it.
out2="$TMP/doctor2.log"
XLINGS_HOME="$TMP/empty" "$MCPP" self doctor > "$out2" 2>&1 || true
if grep -qE "<= ?41|bound of 41" "$out2"; then
    echo "FAIL: 41 is reported with no payload present; the test measures nothing"
    exit 1
fi
echo "PASS: without the payload the bound is not 41"

echo "PASS: device toolkit payload-first"
