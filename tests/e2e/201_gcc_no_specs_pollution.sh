#!/usr/bin/env bash
# requires: gcc elf
# 201_gcc_no_specs_pollution.sh — a gcc artifact must carry only the RUNPATH
# entries this build asked for.
#
# mcpp used to write the loader and rpath into the gcc payload's `specs` file
# at install time, and the link command said nothing about either. Two
# consequences, both measured on a developer machine before this test existed:
#
#   1. the compile side (-L, chosen per build) and the run side (specs, frozen
#      at toolchain install) could name different glibc versions, and did as
#      soon as a second one was installed;
#   2. the substitution that wrote those specs had a one-path needle and a
#      two-path replacement, so every home that ever patched the SHARED payload
#      left one entry behind -- 68 of them, all pointing at deleted mktemp
#      directories, in every gcc artifact the machine produced.
#
# The assertion is deliberately about the artifact, not about the specs file:
# what matters is what a user ends up shipping. A RUNPATH entry under /tmp is
# never something a build asked for.
set -euo pipefail

SELF_DIR=$(cd "$(dirname "$0")" && pwd)   # captured before any cd
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME=$HOME/.mcpp

cd "$TMP"
"$MCPP" new prog > /dev/null
cd prog
cat >> mcpp.toml <<'EOF'

[toolchain]
linux = "gcc@16.1.0"
EOF

"$MCPP" build > "$TMP/build.log" 2>&1 || { cat "$TMP/build.log"; echo "build failed"; exit 1; }

# A cold build, because the generated spec lives in target/ and a cold build
# deletes target/. Written the obvious way -- generate during prepare, consume
# during the build -- gcc got a `-specs=` naming a file that had been removed
# in between, and EVERY `--no-cache` build failed with `cannot read spec file`.
# The warm path above cannot see that: the file is still there from last time.
"$MCPP" build --no-cache > "$TMP/cold.log" 2>&1 || {
    cat "$TMP/cold.log"; echo "cold build failed"; exit 1; }

bin=$(find target -type f -name prog -path '*/bin/*' | head -1)
[[ -n "$bin" ]] || { echo "no binary produced"; exit 1; }

# `readelf` may be an xvm shim needing a per-tool pin; use the payload's own.
readelf=$(ls "$MCPP_HOME"/registry/data/xpkgs/xim-x-binutils/*/bin/readelf 2>/dev/null | head -1)
[[ -x "$readelf" ]] || { echo "SKIP: no payload readelf to inspect with"; exit 0; }

runpath=$("$readelf" -d "$bin" 2>/dev/null | grep -iE 'RUNPATH|RPATH' | sed 's/.*\[\(.*\)\]/\1/' || true)

# 1. No path under a temp directory. These can only come from a specs file that
#    some other home patched; no build ever asks for one.
if echo "$runpath" | tr ':' '\n' | grep -q '^/tmp/'; then
    echo "the artifact carries RUNPATH entries from other homes' temp dirs:"
    echo "$runpath" | tr ':' '\n' | grep '^/tmp/' | head -5
    echo "  ...total $(echo "$runpath" | tr ':' '\n' | grep -c '^/tmp/')"
    echo "These come from the payload's specs, which mcpp must no longer write."
    exit 1
fi

# 2. Every entry must exist. A RUNPATH naming a directory that is not there is
#    a cost paid on every symbol resolution for nothing, and a reliable sign
#    that something wrote a path belonging to a different machine state.
missing=0
while IFS= read -r d; do
    [[ -z "$d" ]] && continue
    [[ "$d" == \$ORIGIN* ]] && continue
    [[ -d "$d" ]] || { echo "  RUNPATH entry does not exist: $d"; missing=$((missing+1)); }
done <<< "$(echo "$runpath" | tr ':' '\n')"
[[ "$missing" -eq 0 ]] || { echo "$missing dead RUNPATH entries"; exit 1; }

# 3. The interpreter is the payload's, and the program runs.
file_out=$(file "$bin")
echo "$file_out" | grep -q 'interpreter .*xim-x-glibc' || {
    echo "PT_INTERP is not the payload's glibc:"; echo "$file_out"; exit 1; }
out=$("$bin" 2>&1) || { echo "binary did not run: $out"; exit 1; }
echo "$out" | grep -q 'Hello' || { echo "unexpected output: $out"; exit 1; }

# 4. Nothing loads from the host.
#
# "Does it run" is not this question, and answering the wrong one is how a
# real gap survived: with the compiler's own lib dir missing from RUNPATH,
# artifacts resolved libgcc_s.so.1 from /lib and ran perfectly on any machine
# that has a host toolchain -- which is every developer machine. It failed
# only on CI's throw-away home, and only as `error while loading shared
# libraries`, with no indication of what had gone missing or why.
#
# Asking where each library actually came from makes the same defect visible
# everywhere, host toolchain or not.
# Only lines whose LEFT side is a bare soname count. ldd resolves the program
# interpreter by running the host's loader, so it always reports the
# interpreter as `<payload>/ld-linux-x86-64.so.2 => /lib64/ld-linux-x86-64.so.2`
# -- a statement about ldd, not about the artifact, and a false positive that
# failed this very check on CI. A real dependency has no slash on the left.
host_lib_lines() {
    ldd "$1" 2>/dev/null \
        | grep -E '^[[:space:]]*[^/[:space:]]+ => +(/lib|/usr/lib|/lib64|/usr/lib64)/' \
        || true
}

if command -v ldd > /dev/null 2>&1; then
    host_libs=$(host_lib_lines "$bin")
    if [[ -n "$host_libs" ]]; then
        echo "the artifact loads libraries from the host:"
        echo "$host_libs" | sed 's/^/  /'
        echo "Every shared library a payload build needs is in the payload;"
        echo "reaching outside it means a directory is missing from RUNPATH."
        exit 1
    fi
fi

# ── The same assertions again, in the OTHER link mode ────────────────────
#
# RUNPATH is built by two separate paths -- CLibMode::Sysroot and
# CLibMode::PayloadFirst -- and a machine only ever takes one of them. This
# one has a usable sysroot, so everything above ran through Sysroot mode and
# PayloadFirst went untested here for as long as this file has existed.
#
# It was not academic: PayloadFirst omitted the compiler's own lib dir, so
# artifacts resolved libgcc_s.so.1 from the host. Invisible on any machine
# with a host toolchain; on CI's throw-away home it was `error while loading
# shared libraries` with no further explanation.
#
# A home with payloads but no subos usually has no sysroot to find, which is
# what selects PayloadFirst -- but "usually" is the honest word: where gcc
# reports a baked sysroot that happens to exist (another checkout on the same
# machine), mcpp accepts it as a last resort and this leg runs Sysroot mode a
# second time instead. Which is what it does on the machine these words were
# written on.
#
# So the deterministic guard for this invariant is the unit test
# (tests/unit/test_link_model_runtime_dirs.cpp), which names the mode rather
# than arranging for it. This leg is still worth running: it covers a second
# home shape end to end, and on CI -- where the baked path does not exist --
# it is the real thing.
export MCPP_HOME="$TMP/payload-first-home"
MCPP_INHERIT_CONFIG=0 MCPP_INHERIT_SUBOS=0 \
    source "$SELF_DIR/_inherit_toolchain.sh"

cd "$TMP"
"$MCPP" new pf > /dev/null
cd pf
cat >> mcpp.toml <<'EOF'

[toolchain]
linux = "gcc@16.1.0"
EOF

"$MCPP" build > "$TMP/pf.log" 2>&1 || {
    cat "$TMP/pf.log"; echo "payload-first build failed"; exit 1; }

pfbin=$(find target -type f -name pf -path '*/bin/*' | head -1)
[[ -n "$pfbin" ]] || { echo "no payload-first binary produced"; exit 1; }

pf_host=$(host_lib_lines "$pfbin")
if [[ -n "$pf_host" ]]; then
    echo "the payload-first artifact loads libraries from the host:"
    echo "$pf_host" | sed 's/^/  /'
    echo "RUNPATH was:"
    "$readelf" -d "$pfbin" 2>/dev/null | grep -iE 'RUNPATH|RPATH' | sed 's/^/  /'
    exit 1
fi

"$pfbin" > /dev/null 2>&1 || {
    echo "payload-first binary did not run: $("$pfbin" 2>&1 | head -2)"; exit 1; }

echo "PASS: gcc artifacts carry only the RUNPATH the build asked for and load"
echo "      nothing from the host — in both sysroot and payload-first modes"
