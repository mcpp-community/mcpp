#!/usr/bin/env bash
# requires: macos
# 266_pack_reads_a_macho_program_without_running_it.sh — `mcpp pack` of a
# PROGRAM on macOS packs it, and never runs it.
#
# WHAT IT ONCE DID, WHICH IS WHY THE CRITERION IS A SIDE EFFECT
#
# The non-PE path resolves an artifact's dependency closure by asking the
# dynamic linker:
#
#   LD_TRACE_LOADED_OBJECTS=1 '<binary>'
#
# That variable is glibc's. dyld has never heard of it, so on macOS this did
# not trace anything — IT RAN THE USER'S PROGRAM, and a bundle containing just
# the binary was written and reported as `Packed`. mcpp then refused a Mach-O
# program outright, which this test asserted. Since #634 A3 the closure is
# read from the program's load commands, so the program packs; what must stay
# true is that packing it runs nothing. The program writes a marker file when
# it runs, and the marker must not exist after the pack.
#
# BOTH SIDES, ON THE SAME HOST: the same run also packs a LIBRARY target,
# which takes a different pipeline and must still succeed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p proj/src
cat > proj/src/mathkit.cppm <<'CPP'
export module mathkit;
export namespace mk { int answer(); }
CPP
cat > proj/src/impl.cpp <<'CPP'
module mathkit;
namespace mk { int answer() { return 42; } }
CPP
cat > proj/src/main.cpp <<'CPP'
#include <cstdio>
import mathkit;
int main() {
    // The side effect the criterion reads: a file beside the working directory.
    if (std::FILE* f = std::fopen("ran.marker", "w")) std::fclose(f);
    std::printf("ok=%d\n", mk::answer());
    return 0;
}
CPP
cat > proj/mcpp.toml <<'TOML'
[package]
name    = "proj"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.proj]
kind = "bin"
main = "src/main.cpp"
[targets.mathkit]
kind = "lib"
TOML

cd proj

# ── 1. the program packs, and packing it ran nothing ────────────────────────
"$MCPP" pack proj > pack.log 2>&1 || {
    cat pack.log
    echo "FAIL: mcpp pack refused a Mach-O program whose closure is the OS's alone."
    exit 1; }
if [ -e ran.marker ] || [ -n "$(find target -name ran.marker | head -1)" ]; then
    cat pack.log
    echo "FAIL: packing the program RAN it (ran.marker exists)."
    exit 1
fi
archive="$(find target/dist -maxdepth 1 -name 'proj-0.1.0-*.tar.gz' | head -1)"
[[ -n "$archive" ]] || { cat pack.log; echo "FAIL: no archive under target/dist"; exit 1; }
tar -tzf "$archive" | grep -q '/bin/proj$' || {
    tar -tzf "$archive"; echo "FAIL: the archive has no bin/proj"; exit 1; }
echo "  a Mach-O program packs, and the pack did not run it"

# ── 2. …and a library target on the same host still packs ──────────────────
"$MCPP" pack mathkit > packlib.log 2>&1 || {
    cat packlib.log
    echo "FAIL: a library target does not pack on macOS."
    exit 1; }
# Searched by content: the program's staged tree above shares the package's
# name prefix, so a directory match could pick either.
[[ -n "$(find target/dist -name '*.a' | head -1)" ]] || {
    find target/dist -type f; echo "FAIL: no archive in the library package"; exit 1; }
echo "  a library target still packs"

echo "PASS: mcpp pack reads a Mach-O program without running it, and library packaging is unaffected"
