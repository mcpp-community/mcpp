#!/usr/bin/env bash
# requires: unix-shell
# The device-toolkit report reads a PAYLOAD before it reads the host.
#
# POSIX hosts only: the report is produced there and nowhere else. On Windows
# the doctor does not emit the section at all -- the toolkit payloads in the
# index are Linux builds, and the host-compiler bound a Windows toolkit states
# is an `_MSC_VER` range the report does not yet read. Running this fixture
# there asserts on a section that cannot appear (measured on the Windows
# runner, 2026-09-05).
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
# ⚠️ BOTH GUARDS, AND THAT IS NOT BELT-AND-BRACES. The report names the bound
# for the family of the compiler mcpp resolved, and that family differs by
# platform: gcc on the Linux runners, clang on the macOS one. A fixture stating
# only the GNU guard reported `clang N <= 0` there — a criterion decided by the
# environment rather than by the code under test. Stating both makes the
# reading 41 whichever family answers.
cat > "$PAYLOAD/host_config.h" <<'HDR'
#if __GNUC__ > 41
#error -- unsupported GNU version! gcc versions later than 41 are not supported!
#endif
#if defined(__clang__)
#error -- unsupported clang version! clang version must be less than 42 and greater than 3.2 .
#endif
HDR

# ⚠️ BOTH STORES ISOLATED. The report searches mcpp's own store before the
# xlings one, so on a machine that has a real CUDA payload installed the real
# one answers and this fixture is never read — the assertion then measures
# whatever that machine happens to have. Fresh homes make the reading a property
# of the code rather than of the runner.
#
# OFFLINE, because a fresh home is otherwise bootstrapped in full during the
# diagnosis: measured on the development machine, the doctor cloned the index,
# installed ninja and patchelf, and then downloaded glibc and gcc -- 1.4 GB and
# 229 seconds -- before reaching the section under test. Under `MCPP_OFFLINE`
# the bootstrap is skipped and the toolchain is not provisioned; the device
# toolkit section is produced either way, and its reading is what matters here.
out="$TMP/doctor.log"
MCPP_HOME="$TMP/mcpp" XLINGS_HOME="$TMP/xlings" MCPP_OFFLINE=1 \
    "$MCPP" self doctor > "$out" 2>&1 || true

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
MCPP_HOME="$TMP/mcpp2" XLINGS_HOME="$TMP/empty" MCPP_OFFLINE=1 \
    "$MCPP" self doctor > "$out2" 2>&1 || true
if grep -qE "<= ?41|bound of 41" "$out2"; then
    echo "FAIL: 41 is reported with no payload present; the test measures nothing"
    exit 1
fi
echo "PASS: without the payload the bound is not 41"

echo "PASS: device toolkit payload-first"
