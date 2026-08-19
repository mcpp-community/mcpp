#!/usr/bin/env bash
# requires: pack patchelf elf python3
# 215_pack_has_no_build_machine_paths.sh — a bundle must not depend on the
# machine that built it, and its executable must carry DT_RPATH.
#
# TWO DEFECTS, ONE FILE.
#
#  1. Only the main executable's search path was rewritten. Every bundled .so
#     kept the RUNPATH it was LINKED with, and in this ecosystem that is a list
#     of absolute paths into the build machine's xlings store:
#
#        <store>/xim-x-glibc/2.44/lib64 : <store>/xim-x-gcc/16.1.0/lib64
#        : <store>/compat-x-glx-runtime/…/lib : $ORIGIN
#
#     "Depends on the xlings ecosystem" would be a design choice; "depends on
#     THIS machine's store" is a defect — and it is invisible, because the
#     bundle runs perfectly where it was built.
#
#  2. `patchelf --set-rpath` writes DT_RUNPATH by default. For an executable
#     that is the graphics defect one layer later: DT_RUNPATH is not consulted
#     for a dlopen() performed by another object, and every dlopen in the GL
#     stack is performed by another object.
#
# The assertion sweeps EVERY ELF in the bundle, not just the binary — the
# first defect lived precisely in the ones nobody looked at.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

# The ELF reader lives in _elf_tag.sh — 264 asks the same question of a
# LIBRARY package, and two copies of this parser would be two definitions of
# what "carries a build-machine path" means.
source "$(dirname "$0")/_elf_tag.sh"

# The bundle must actually CONTAIN a library, or this test sweeps one
# executable and proves nothing about the defect it exists for — which lived
# entirely in the objects nobody looked at.
#
# `force_bundle` on libgcc_s is the cheapest way to get one, and it is not a
# contrivance: it resolves to a file inside the build machine's xlings store,
# carrying that store's paths in its own RUNPATH. That is the defect's exact
# shape. (The project's own `kind = "shared"` target does NOT work here — mcpp
# links module objects into the executable, so it is never a DT_NEEDED.)
cd "$TMP"
"$MCPP" new bundled > /dev/null
cd bundled
cat >> mcpp.toml <<'EOF'

[pack.bundle-project]
force_bundle = ["libgcc_s.so.1"]
EOF

"$MCPP" pack > "$TMP/pack.log" 2>&1 || { cat "$TMP/pack.log"; exit 1; }

TARBALL="$(ls target/dist/*.tar.gz | head -1)"
[[ -n "$TARBALL" ]] || { echo "FAIL: no tarball"; cat "$TMP/pack.log"; exit 1; }
mkdir -p "$TMP/x" && tar -xzf "$TARBALL" -C "$TMP/x"

# EVERYTHING under the mcpp home is machine-local, not just the store.
#
# This used to be `$MCPP_HOME/registry/data/xpkgs`, which is a SUBSET of what
# the sweep claims to cover — and the gap was load-bearing. The SubOS farm
# (`<home>/registry/subos/<name>/lib`) now lands in the artifact's DT_RPATH by
# design, and it sits outside `data/xpkgs`, so a leaked farm path would have
# walked straight past this guard while the file's own title says no
# build-machine paths survive. A check narrower than its claim reports "clean"
# for the one thing it cannot see.
STORE="$(cd "$MCPP_HOME" 2>/dev/null && pwd || true)"

fail=0
found_exe=0
found_lib=0
while IFS= read -r obj; do
    head -c4 "$obj" 2>/dev/null | grep -q $'\x7fELF' || continue
    read -r form tag paths <<<"$(read_tag "$obj")"
    [[ "$form" == "NOT-ELF64" ]] && continue
    printf '  %-40s %-14s %s %s\n' "${obj#$TMP/x/}" "$form" "$tag" "$paths"

    if [[ -n "$STORE" && "$paths" == *"$STORE"* ]]; then
        echo "FAIL: bundled object still points at the BUILD MACHINE's store"
        echo "      $obj"
        echo "      $paths"
        fail=1
    fi
    if [[ "$form" == "executable" ]]; then
        found_exe=1
        case "$tag" in
            RPATH|NONE) ;;
            *)
                echo "FAIL: bundled executable carries $tag, contract requires DT_RPATH"
                echo "      a bundled vendor library cannot be reached through DT_RUNPATH"
                echo "      when the dlopen is performed by something else"
                fail=1
                ;;
        esac
    else
        found_lib=1
        case "$tag" in
            RUNPATH|NONE) ;;
            *)
                echo "FAIL: bundled library carries $tag, contract requires DT_RUNPATH"
                fail=1
                ;;
        esac
    fi
done < <(find "$TMP/x" -type f)

[[ "$found_exe" == "1" ]] || { echo "FAIL: swept no executable — the test proved nothing"; exit 1; }
# The load-bearing coverage guard: the defect this file exists for was in the
# BUNDLED LIBRARIES, so a run that found none has not tested it.
[[ "$found_lib" == "1" ]] || {
    echo "FAIL: the bundle contains no shared library, so the defect this test"
    echo "      targets (bundled .so keeping build-machine paths) was not exercised"
    find "$TMP/x" -type f | sed "s|$TMP/x/||"
    exit 1
}
[[ "$fail" == "0" ]] || exit 1

# A program with no host capabilities must NOT get a requirements file: an
# empty one would be read as a claim that nothing is needed.
if find "$TMP/x" -name HOST-REQUIREMENTS | grep -q .; then
    echo "FAIL: a program with no host capabilities got a HOST-REQUIREMENTS file"
    exit 1
fi

# ── the mode that bundles NOTHING ───────────────────────────────────────────
#
# `--mode system` is the one mode that ships no libraries, and until now no
# test packed with it — so nothing checked the one thing it must get right:
# an artifact that carries no dependencies must also carry no addressing for
# them. It links against a private glibc with the SubOS farm in its DT_RPATH,
# and both of those are this machine's alone.
"$MCPP" pack --mode system > "$TMP/pack-system.log" 2>&1 || {
    cat "$TMP/pack-system.log"; exit 1; }
SYSTAR="$(ls target/dist/*-system.tar.gz | head -1)"
[[ -n "$SYSTAR" ]] || { echo "FAIL: no system-mode tarball"; exit 1; }
mkdir -p "$TMP/xs" && tar -xzf "$SYSTAR" -C "$TMP/xs"

sysfail=0
sysfound=0
while IFS= read -r obj; do
    head -c4 "$obj" 2>/dev/null | grep -q $'\x7fELF' || continue
    read -r form tag paths <<<"$(read_tag "$obj")"
    [[ "$form" == "NOT-ELF64" ]] && continue
    sysfound=1
    printf '  [system] %-30s %-14s %s %s\n' "${obj#$TMP/xs/}" "$form" "$tag" "$paths"
    if [[ -n "$STORE" && "$paths" == *"$STORE"* ]]; then
        echo "FAIL: --mode system artifact points at the BUILD MACHINE"
        echo "      $obj"
        echo "      $paths"
        sysfail=1
    fi
done < <(find "$TMP/xs" -type f)
[[ "$sysfound" == "1" ]] || { echo "FAIL: --mode system swept no ELF"; exit 1; }
[[ "$sysfail" == "0" ]] || exit 1

echo "PASS: no build-machine paths survive packing (vendored or system), and every object carries its contract tag"
