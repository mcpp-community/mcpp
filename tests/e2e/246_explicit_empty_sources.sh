#!/usr/bin/env bash
# requires:
# (no capability: the probe below greps a FILENAME out of build.ninja, not a
#  compiler flag, so nothing here is toolchain-specific. It carried
#  `# requires: gcc` for one round out of copy-paste, and that capability is
#  Linux-only by design — it skipped on macOS and Windows for no reason.)
# 246_explicit_empty_sources.sh — `sources = []` means "compile nothing", and
# omitting the key still means "the default glob".
#
# They used to be byte-identical: the parser filled the default glob whenever
# the vector was empty, so an author had NO spelling for "nothing". A binary
# distribution package needs one — a header-only package compiles nothing, and
# any file left under `src/` would otherwise be swept up and compiled into the
# consumer's build, where it can collide with the symbols the prebuilt library
# already defines.
#
# The probe is a leftover source that must NOT be compiled in one case and MUST
# be in the other. Asserting only the first would pass against a parser that
# ignores `sources` entirely.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p probe/src
echo 'int mcpp_leftover_probe(void) { return 1; }' > probe/src/leftover.cpp
echo 'int main() { return 0; }' > probe/main.cpp

write_manifest() {   # $1 = the [build] body
    cat > probe/mcpp.toml <<EOF
[package]
name    = "probe"
version = "0.1.0"
[build]
$1
[targets.probe]
kind = "bin"
main = "main.cpp"
EOF
}

hits() {
    local nj; nj="$(find probe/target -name build.ninja | head -1)"
    grep -c 'leftover' "$nj" || true
}

# ── explicitly empty: nothing is compiled ──────────────────────────────
write_manifest 'sources = []'
rm -rf probe/target
( cd probe && "$MCPP" build > empty.log 2>&1 ) || { cat probe/empty.log; echo "build failed"; exit 1; }
n="$(hits)"
[[ "$n" -eq 0 ]] || {
    echo "FAIL: 'sources = []' still compiled the leftover source ($n references)"
    exit 1; }

# ── absent: the default glob still applies ─────────────────────────────
write_manifest '# no sources key at all'
rm -rf probe/target
( cd probe && "$MCPP" build > default.log 2>&1 ) || { cat probe/default.log; echo "build failed"; exit 1; }
n="$(hits)"
[[ "$n" -gt 0 ]] || {
    echo "FAIL: omitting 'sources' stopped applying the default glob"
    exit 1; }

echo "PASS: an explicitly empty sources list is distinguishable from an absent one"
