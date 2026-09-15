#!/usr/bin/env bash
# requires: windows
# 705 -- a measurement leg (#646 F2, #649 E10): on the MSVC ABI with the default
# toolchain, every DLL links its own C and C++ runtime. This prints, for a
# program over a C++ DLL under three runtime statements, whether a standard
# exception thrown in the DLL is caught by its class, whether the DLL's own
# exception class is, and whether an `std::error_code` compares equal across the
# boundary. See `_cxx_identity_across_images_body.sh`.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

IMAGE_FORMAT=pe
source "$(dirname "$0")/_cxx_identity_across_images_body.sh"

echo "PASS: 705 C++ identity across PE images was measured"
