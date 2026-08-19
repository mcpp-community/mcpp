#!/usr/bin/env bash
# requires: macos
# 266_pack_refuses_a_macho_program.sh — `mcpp pack` of a PROGRAM on macOS is
# refused, and refused for the stated reason.
#
# WHAT IT USED TO DO INSTEAD, WHICH IS WORSE THAN FAILING
#
# The non-PE path resolves an artifact's dependency closure by asking the
# dynamic linker:
#
#   LD_TRACE_LOADED_OBJECTS=1 '<binary>'
#
# That variable is glibc's. dyld has never heard of it (its counterpart is
# `DYLD_PRINT_LIBRARIES`), so on macOS this does not trace anything — IT RUNS
# THE USER'S PROGRAM. Whatever the program prints is then parsed as a
# dependency table, yields nothing, and a bundle containing just the binary is
# written and reported as `Packed`. A program with side effects performs them.
# An interactive one hangs the packer.
#
# The `_WIN32` branch beside it has refused the same class since it was
# written; macOS was simply never checked, because the e2e harness only grants
# the `pack` capability where `elf` + `patchelf` are both present — i.e. Linux.
# So no job in this suite has ever run `mcpp pack` on a Mac.
#
# ⚠️ BOTH SIDES, ON THE SAME HOST. Asserting only the refusal cannot tell "the
# gate works" from "pack is broken on this machine". So the same run also packs
# a LIBRARY target, which takes a different pipeline and must still succeed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p proj/src
cat > proj/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > proj/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF
cat > proj/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
cat > proj/mcpp.toml <<'EOF'
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
EOF

cd proj

# ── 1. the program is refused, by name ─────────────────────────────────────
if "$MCPP" pack proj > pack.log 2>&1; then
    cat pack.log
    echo "FAIL: mcpp pack produced a bundle for a Mach-O program."
    echo "      Its dependency closure cannot be resolved on this platform, so"
    echo "      whatever it produced is not one — and producing it RAN the program."
    exit 1
fi
grep -qi "Mach-O" pack.log || {
    cat pack.log
    echo "FAIL: pack failed, but not with the Mach-O refusal — so this test is"
    echo "      observing some other failure and proves nothing about the gate."
    exit 1; }
grep -q "LD_TRACE_LOADED_OBJECTS" pack.log || {
    cat pack.log
    echo "FAIL: the refusal does not say WHY. A reader has to be able to tell"
    echo "      this from 'macOS is unsupported in general'."
    exit 1; }
echo "  a Mach-O program is refused, and the message names the mechanism"

# ── 2. …and a library target on the same host still packs ──────────────────
"$MCPP" pack mathkit > packlib.log 2>&1 || {
    cat packlib.log
    echo "FAIL: a library target does not pack on macOS either — the refusal above"
    echo "      is therefore not evidence of a working gate."
    exit 1; }
pkg="$(find target/dist -maxdepth 1 -type d -name 'proj-0.1.0-*' | head -1)"
[[ -n "$(find "$pkg" -name '*.a' | head -1)" ]] || {
    find "$pkg" -type f; echo "FAIL: no archive in the library package"; exit 1; }
echo "  a library target still packs"

echo "PASS: mcpp pack refuses a Mach-O program with the reason, and library packaging is unaffected"
