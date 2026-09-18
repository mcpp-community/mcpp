#!/usr/bin/env bash
# requires: gcc mingw-cross
# 742 -- a compiler is refused for a declared [c-abi] block only when
# REALISING it actually needs Clang-specific tokens, not merely because the
# block is declared.
#
# THE REGRESSION THIS PINS (coordinator report, found by openkal-musl 0.15.0's
# own CI, an hour after 2026.9.18.1 shipped it). Before this fix the gate ran
# BEFORE `cenv::realise`: any non-Clang compiler was refused the instant a
# [c-abi] block existed anywhere in the graph, regardless of what realising
# it would actually require. openkal-musl's own declaration --
# `presents = "posix", data-model = "arch-default", wchar = 32,
# builtins = "iso"` -- is exactly what x86_64 Linux's OWN default already is:
# realising it needs no substitution at all (docs/22's mapping table, the
# Linux row). GCC was refused anyway, for a substitution nothing asked it to
# perform -- every GCC user of openkal-musl on Linux lost the package for
# nothing.
#
# The fix, and this test's two legs: `cenv::realise` runs FIRST (it is a pure
# function of the declaration and the target, not of the compiler); the
# Clang-only gate runs SECOND and fires only when the realisation is
# non-empty. Leg A is the regression itself, reproduced and pinned green.
# Leg B is the case that must NOT have changed: Windows realises a non-empty
# Cygwin-flavoured substitution, so GCC (mingw) there is still refused with
# the same diagnostic as before -- this is the openkal-musl-measured case
# (MinGW's `long` is 32-bit regardless of flags), not a hole this fix opens.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p fakemusl/src src
cat > fakemusl/src/lib.c <<'EOF'
int fakemusl_marker(void) { return 0; }
EOF
cat > src/main.c <<'EOF'
int main(void) { return 0; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "gcc-cabi-probe"
version = "0.1.0"

[dependencies]
fakemusl = { path = "fakemusl" }

[build]
allow_host_libs = true
EOF
# openkal-musl 0.15.0's OWN declaration, verbatim.
cat > fakemusl/mcpp.toml <<'EOF'
[package]
name     = "fakemusl"
version  = "0.15.0"
provides = ["mcpp:c-abi=musl"]

[targets.fakemusl]
kind    = "lib"
sources = ["src/*.c"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
builtins   = "iso"
EOF

# No `--toolchain` flag on either leg: GCC is already this project's default
# toolchain and the default toolchain for both targets below (`mcpp toolchain
# list`), so naming it explicitly would test a CLI override this defect never
# involved -- the regression was in the DEFAULT path an ordinary `mcpp build`
# takes.

# ── A. GCC on Linux: the realisation is empty, so GCC must be ACCEPTED ─────
out=$("$MCPP" build --target x86_64-linux-gnu 2>&1) || {
    echo "FAIL: GCC must be accepted when the [c-abi] realisation is empty" >&2
    echo "$out" >&2
    exit 1
}
echo "$out" | grep -qi "is not one mcpp can realise\|requires Clang-specific" && {
    echo "FAIL: GCC/Linux was refused for a substitution nothing asked it to perform" >&2
    echo "$out" >&2
    exit 1
}
echo "OK: A (GCC/Linux accepted, empty realisation)"

# ── B. GCC (mingw) on Windows: the realisation is non-empty, still refused ─
rm -rf target
out=$("$MCPP" build --target x86_64-windows-gnu 2>&1) && {
    echo "FAIL: GCC (mingw) on Windows must still be refused -- the Cygwin" \
         "substitution there is non-empty and this fix must not touch it" >&2
    echo "$out" >&2
    exit 1
}
echo "$out" | grep -qi "requires Clang-specific" || {
    echo "FAIL: the refusal must still name the Clang-specific requirement" >&2
    echo "$out" >&2
    exit 1
}
echo "OK: B (GCC/Windows still refused, non-empty realisation)"

echo "OK"
