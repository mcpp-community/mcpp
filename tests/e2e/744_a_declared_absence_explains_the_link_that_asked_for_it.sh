#!/usr/bin/env bash
# requires: gcc
# 744 -- a `[c-abi-absent]` row with `form = "link"` reaches the LINK, and the
# note it produces is read by a person.
#
# WHAT WAS UNTESTED, AND WHAT IT COST. `c_abi_absent_facility_advice` had nine
# unit tests covering the matching rules and the sidecar round-trip, and the
# table had seven covering the manifest. Nothing ran the two together: that a
# real build WRITES the sidecar beside build.ninja, that a real link failure
# READS it back, and that what arrives is a sentence.
#
# It was not a sentence. The C library's name was interpolated from two
# separate conditionals -- an opening paren, then the name -- and the closing
# paren was never emitted, so every reader saw
#
#     note: the C library in this graph (musl declares that it does not supply
#
# The unit test asserted `find("musl")`, which is true of that spelling too.
# A criterion aimed at a substring of a sentence cannot see the sentence.
#
# THE SYMBOL IS DELIBERATELY ONE NOTHING DEFINES. `fork` is the real row in
# openkal-musl's manifest and is defined by every C library on a Linux host,
# so a test written with it would link and assert nothing.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p libc/src src
cat > libc/src/lib.c <<'EOF'
int fake_c_library_marker(void) { return 0; }
EOF
cat > src/main.c <<'EOF'
extern int mcpp_absent_probe_fn(void);
int main(void) { return mcpp_absent_probe_fn(); }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "absence-explains-the-link"
version = "0.1.0"

[targets.absence-explains-the-link]
kind = "bin"
main = "src/main.c"

[dependencies]
fakelibc = { path = "libc" }

[build]
allow_host_libs = true
EOF
cat > libc/mcpp.toml <<'EOF'
[package]
name     = "fakelibc"
version  = "0.1.0"
provides = ["mcpp:c-abi=musl"]

[targets.fakelibc]
kind    = "lib"
sources = ["src/*.c"]

[c-abi-absent]
mcpp_absent_probe_fn = { form = "link", note = "this environment has no such operation" }
EOF

out="$("$MCPP" build 2>&1)" && {
    echo "FAIL: the link must fail -- nothing defines mcpp_absent_probe_fn" >&2
    echo "$out" >&2
    exit 1
}

# ── A. the linker's own message is still there, unreplaced ─────────────────
echo "$out" | grep -qi "mcpp_absent_probe_fn" || {
    echo "FAIL: the linker's own diagnostic must survive" >&2
    echo "$out" >&2
    exit 1
}
echo "OK: A (the linker still reports the symbol)"

# ── B. the manifest row reached the link ───────────────────────────────────
echo "$out" | grep -q "this environment has no such operation" || {
    echo "FAIL: the [c-abi-absent] row's note must reach the link diagnostic." \
         "The sidecar is written beside build.ninja during the build and read" \
         "back when the link fails; one of those two did not happen." >&2
    echo "$out" >&2
    exit 1
}
echo "OK: B (the row's note reached the failure)"

# ── C. the sentence reads as a sentence ────────────────────────────────────
# THE WHOLE CLAUSE, NOT A SUBSTRING OF IT. This is the assertion the unit
# test did not make, and the defect it did not see.
echo "$out" | grep -q "the C library in this graph (musl) declares that it does not supply" || {
    echo "FAIL: the note must name the C library in a closed parenthetical." >&2
    echo "     got: $(echo "$out" | grep -m1 'the C library in this graph')" >&2
    exit 1
}
echo "OK: C (the note names the C library and reads as a sentence)"

# ── D. an absence is not reported as a defect in the build ─────────────────
echo "$out" | grep -q "not a defect in the build" || {
    echo "FAIL: the note must say an absence is a property of the environment" >&2
    echo "$out" >&2
    exit 1
}
echo "OK: D (the note distinguishes an absence from a defect)"

echo "OK"
