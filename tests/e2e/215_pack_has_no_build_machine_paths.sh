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

read_tag() {
python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\x7fELF' or d[4] != 2:
    print("NOT-ELF64"); raise SystemExit
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
rpath = runpath = interp = False
paths = []
dynoff = dynsz = None
for i in range(phnum):
    off = phoff + i * phentsize
    ptype, = struct.unpack_from('<I', d, off)
    if ptype == 3: interp = True
    if ptype == 2:
        dynoff, = struct.unpack_from('<Q', d, off + 0x08)
        dynsz,  = struct.unpack_from('<Q', d, off + 0x20)
if dynoff is None:
    print(("executable" if interp else "shared_library"), "NONE", ""); raise SystemExit
# locate DT_STRTAB so the path strings can be read back
strtab_addr = strtab_off = None
entries = []
j = dynoff
while j < dynoff + dynsz:
    tag, val = struct.unpack_from('<qQ', d, j)
    if tag == 0: break
    entries.append((tag, val))
    if tag == 5: strtab_addr = val
    j += 16
if strtab_addr is not None:
    for i in range(phnum):
        off = phoff + i * phentsize
        ptype, = struct.unpack_from('<I', d, off)
        if ptype != 1: continue                    # PT_LOAD
        p_off,  = struct.unpack_from('<Q', d, off + 0x08)
        p_vaddr,= struct.unpack_from('<Q', d, off + 0x10)
        p_filesz,=struct.unpack_from('<Q', d, off + 0x20)
        if p_vaddr <= strtab_addr < p_vaddr + p_filesz:
            strtab_off = p_off + (strtab_addr - p_vaddr)
            break
for tag, val in entries:
    if tag in (15, 29):
        if tag == 15: rpath = True
        else: runpath = True
        if strtab_off is not None:
            end = d.index(b'\0', strtab_off + val)
            paths.append(d[strtab_off + val:end].decode('utf-8', 'replace'))
form = "executable" if interp else "shared_library"
tag = ("BOTH" if rpath and runpath else
       "RPATH" if rpath else "RUNPATH" if runpath else "NONE")
print(form, tag, ":".join(paths))
PY
}

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

# The build machine's store prefix. Everything under it is machine-local by
# construction, so its presence in a shipped artifact is the defect itself.
STORE="$(cd "$MCPP_HOME/registry/data/xpkgs" 2>/dev/null && pwd || true)"

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

echo "PASS: no build-machine paths survive packing, and every object carries its contract tag"
