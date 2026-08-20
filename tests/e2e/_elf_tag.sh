#!/usr/bin/env bash
# _elf_tag.sh — read an ELF's FORM and its loader search-path TAG.
#
# `read_tag <file>` prints three fields:
#
#     <form> <tag> <paths>
#
#   form   executable | shared_library | NOT-ELF64
#   tag    RPATH | RUNPATH | BOTH | NONE
#   paths  ':'-joined contents of whichever tag is present
#
# ⚠️ WHY A PARSER AND NOT `strings`
#
# Removing DT_RPATH/DT_RUNPATH removes the ENTRY, not the string it pointed at:
# `.dynstr` is tail-merged by the linker, so a shorter live string can begin
# inside the dead one and deleting those bytes cannot be shown safe. Measured:
# `patchelf --remove-rpath` leaves the identical residue at the identical file
# size, so this is the reference behaviour, not mcpp's shortcut.
#
# Consequence: "this artifact carries no build-machine path" is a question about
# the DYNAMIC ENTRIES. A `grep` over the file's bytes answers a different
# question and reports a correctly relocated artifact as dirty — and it would
# also flip to "clean" for an unrelated reason the day `mcpp pack` started
# stripping DWARF. One check, one thing.
#
# Shared by 215 (application bundles) and 264 (library packages) so the two
# cannot drift into disagreeing about what the criterion is.

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
