#!/usr/bin/env bash
# requires: gcc
# The target triple states a request; the target side states the fact.
#
# WHY THIS FILE EXISTS.
#
# Two symptoms, one cause. A triple serves as an IDENTITY — the output
# directory, a cache key, the subject of a `cfg()` — and identities must be
# total, so `parse` fills `x86_64-linux` in as `x86_64-linux-gnu`. It also
# serves as a REQUEST, and a request must be able to say nothing. The filling
# destroyed the second, and the target row's convention was applied before the
# graph that decides whether it is needed even exists.
#
# Measured before this:
#
#     Target x86_64-linux-gnu → x86_64-unknown-linux-gnu
#            c-abi   musl   (openkal-musl@0.3.3, graph)
#
# — the name contradicts the line under it, and the build succeeded anyway.
set -e

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p libc/src app/src
cat > libc/mcpp.toml <<'TOML'
[package]
namespace = "probe"
name      = "tiny-musl"
version   = "0.1.0"
provides  = ["mcpp:c-abi=musl"]

[build]
sources = []
TOML
cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
tiny-musl = { path = "../libc" }
TOML
printf 'int main(){ return 0; }\n' > app/src/main.cpp
cd app

# ── 1. Declining to name a C library is not naming `gnu` ────────────────────
#
# `|| true`, and that is the subject rather than a concession: the stand-in
# package declares the layer without supplying one, so the LINK afterwards has
# no C library to find. What this file asserts — the report and the refusal —
# is complete before a single object is compiled.
out="$("$MCPP" build --target x86_64-linux 2>&1 || true)"
grep -q "requests the" <<< "$out" && {
    echo "declining to name a C library must not be a contradiction:"
    echo "$out"; exit 1; }
grep -qE "Target x86_64-linux( |$)" <<< "$out" || {
    echo "the report must show the target as the project spelled it:"; echo "$out"; exit 1; }
grep -q "Target x86_64-linux-gnu" <<< "$out" && {
    echo "a segment the project did not write must not appear in the report:"
    echo "$out"; exit 1; }

# ── 2. Naming one the graph disagrees with is reported, not refused ─────────
#
# The severity was decided by a measurement. Refusing is the clean answer —
# only one of the two names can describe the artifact — and it broke every
# project and CI configuration spelling the host target `x86_64-linux-gnu`,
# which is what `mcpp toolchain list` prints and therefore what people write.
# mcpp's own openkal matrix was the first casualty.
#
# What settles it is that the request changes nothing: the graph supplies the C
# library either way, so the segment is ignored rather than violated.
out="$("$MCPP" build --target x86_64-linux-gnu 2>&1 || true)"
grep -q "asks for the .gnu. C ABI" <<< "$out" || {
    echo "the mismatch was not reported:"; echo "$out"; exit 1; }
grep -q "musl" <<< "$out" || {
    echo "the report does not name what resolved:"; echo "$out"; exit 1; }
grep -q -- "--target x86_64-linux" <<< "$out" || {
    echo "the report names no correct spelling to use instead:"; echo "$out"; exit 1; }

echo "OK"
