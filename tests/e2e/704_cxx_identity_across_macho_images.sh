#!/usr/bin/env bash
# requires: macos
# 704 -- a measurement leg (#646 F2): on Mach-O, the payload's default contract
# gives every dylib a private libc++ linked with `-load_hidden`. This prints,
# for a program over a C++ dylib under three runtime statements, whether a
# standard exception thrown in the dylib is caught by its class, whether the
# dylib's own exception class is, and whether an `std::error_code` compares
# equal across the boundary. See `_cxx_identity_across_images_body.sh`.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

IMAGE_FORMAT=macho
source "$(dirname "$0")/_cxx_identity_across_images_body.sh"

echo "PASS: 704 C++ identity across Mach-O images was measured"
