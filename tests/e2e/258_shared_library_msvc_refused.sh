#!/usr/bin/env bash
# requires: msvc
# 258_shared_library_msvc_refused.sh — `kind = "shared"` on the MSVC ABI is
# refused, and the message says why.
#
# The refusal is NOT about the linker. `link /DLL /IMPLIB:` has been in mcpp's
# rule table all along. It is about symbol export: MSVC exports nothing from a
# DLL unless the source says `__declspec(dllexport)` or a `.def` file lists the
# symbols. Without that the import library comes out EMPTY and every consumer
# fails with unresolved externals naming symbols that are plainly in the object
# files — a diagnostic pointing nowhere near its cause. Producing that is worse
# than refusing.
#
# ⚠️ WHY THIS TEST IS SEPARATE FROM 257. On a Linux host this cannot be observed:
# the machine cannot serve `x86_64-windows-msvc`, so the target-vocabulary gate
# answers first and make_plan is never reached. A `# requires:`-less version of
# this test would assert a message its host cannot produce.
#
# ⚠️ AND WHY IT PINS BOTH SIDES. `kind = "lib"` must still build in the same
# project with the same toolchain. Asserting only the refusal cannot distinguish
# "shared is refused" from "this project does not build at all".
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF

manifest() {   # $1 = kind
    cat > "$TMP/mathkit/mcpp.toml" <<EOF
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "$1"
[toolchain]
windows = "msvc@system"
EOF
}

cd mathkit

# ── kind = "shared" is refused, naming the actual obstacle ──────────────
manifest shared
rm -rf target
if "$MCPP" build > shared.log 2>&1; then
    echo "FAIL: a kind=\"shared\" target built for the MSVC ABI."
    find target -name '*.dll' -o -name '*.lib' | head
    echo "      If the import library is empty, consumers fail with unresolved"
    echo "      externals for symbols that are visibly present in the objects."
    exit 1
fi
grep -qi 'dllexport' shared.log || {
    cat shared.log
    echo "FAIL: refused, but not by the shared-library gate — the message must"
    echo "      name symbol export, or the reader cannot tell what to do about it."
    exit 1; }
# And it must point somewhere: a refusal with no way forward is a dead end.
grep -q 'windows-gnu' shared.log || {
    cat shared.log
    echo "FAIL: the refusal names no alternative. MinGW auto-exports, and that is"
    echo "      the answer for anyone who actually needs a DLL here."
    exit 1; }

# ── and the static form still builds, same project, same toolchain ──────
manifest lib
rm -rf target
"$MCPP" build > lib.log 2>&1 || {
    cat lib.log
    echo "FAIL: kind=\"lib\" does not build either, so the refusal above proves"
    echo "      nothing about shared libraries specifically."
    exit 1; }
[[ -n "$(find target -name 'mathkit.lib' | head -1)" ]] || {
    find target -type f | head
    echo "FAIL: no mathkit.lib — the MSVC static path did not produce its artifact"
    exit 1; }

echo "PASS: MSVC refuses kind=\"shared\" for the export reason, and still builds kind=\"lib\""
