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

echo "PASS: gcc artifact carries only the RUNPATH this build asked for"
