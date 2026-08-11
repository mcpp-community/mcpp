#!/usr/bin/env bash
# requires: gcc elf python3
# 219_runtime_search_farm_is_last.sh — the run-time search closure, and the one
# invariant in it.
#
# WHAT IS BEING FIXED
#
# mcpp already treats the selected SubOS as its sysroot: `--sysroot=<subos>` is
# on the compile AND link lines, so `-lGL` resolves out of `<subos>/lib` with
# no flags from the user. Nothing carried that view into the RUN-time search
# path, which was built from toolchain payload directories alone. The result
# was a link that succeeded and an executable that could not start.
#
# THE INVARIANT
#
#   payload directories first, the SubOS farm LAST
#
# and it is about mutability, not taste. `<subos>/lib` is a symlink view
# rewritten by every `xlings install`; a payload directory is written once.
# Payload-first keeps libc / libm / libstdc++ resolving from the pinned payload
# and leaves the farm to supply only what nothing else does. Farm-first would
# let a later install silently change which libc an ALREADY LINKED artifact
# loads — a failure that appears long after the build that caused it.
#
# WHY THIS ASSERTS THE ARTIFACT AND THE RECORD
#
# They fail differently. A wrong DT_RPATH is a regression in what mcpp emits; a
# record that disagrees with the DT_RPATH means resolution.json is describing a
# build that did not happen, and every downstream reader (`mcpp why runtime`,
# `mcpp pack`, CI) is then reasoning about fiction.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > mcpp.toml <<'EOF'
[package]
name = "closure"
version = "0.1.0"
EOF
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

RES="$(ls target/*/*/resolution.json 2>/dev/null | head -1)"
BIN="$(ls target/*/*/bin/closure 2>/dev/null | head -1)"
[[ -n "$RES" ]] || { echo "FAIL: no resolution.json"; exit 1; }
[[ -n "$BIN" ]] || { echo "FAIL: no executable"; ls -R target | head -30; exit 1; }

# ── the recorded closure ────────────────────────────────────────────────────
python3 - "$RES" > "$TMP/closure.txt" <<'PY' || exit 1
import json, sys
doc = json.load(open(sys.argv[1]))
search = doc["runtime"].get("search", {})
closure = search.get("closure")
if closure is None:
    print("MISSING", file=sys.stderr)
    raise SystemExit("FAIL: resolution.json records no runtime search closure")
for d in closure:
    print(d["origin"], d["path"], sep="\t")
PY
cat "$TMP/closure.txt"

FARM="$(awk -F'\t' '$1=="subos_farm"{print $2}' "$TMP/closure.txt" | tail -1)"
PAYLOAD_FIRST="$(awk -F'\t' 'NR==1{print $1}' "$TMP/closure.txt")"

if [[ -z "$FARM" ]]; then
    # SKIP, LOUDLY. A SubOS with no lib view is a legitimate configuration
    # (a bare or freshly created one), but a test that silently passes in that
    # state would report "the farm is last" on a machine that has no farm.
    echo "SKIP: this SubOS exposes no library view, so there is no farm entry"
    echo "      to order. Recorded closure:"
    cat "$TMP/closure.txt"
    exit 0
fi

# ── invariant 1: the closure is ordered, farm last ──────────────────────────
LAST_ORIGIN="$(tail -1 "$TMP/closure.txt" | cut -f1)"
[[ "$LAST_ORIGIN" == "subos_farm" ]] || {
    echo "FAIL: the last entry of the search closure is '$LAST_ORIGIN', not the farm"
    echo "      A mutable symlink view ahead of an immutable payload lets a later"
    echo "      \`xlings install\` change which libc an already-linked artifact loads."
    cat "$TMP/closure.txt"
    exit 1
}
[[ "$PAYLOAD_FIRST" == "payload" ]] || {
    echo "FAIL: the closure does not start with a payload directory (got '$PAYLOAD_FIRST')"
    cat "$TMP/closure.txt"
    exit 1
}

# ── invariant 2: the artifact says the same thing ───────────────────────────
#
# Parsed from the dynamic section directly rather than via readelf: a sandbox
# home does not reliably have binutils, and on at least one real machine its
# shims pointed into a deleted directory.
DT_RPATH="$(python3 - "$BIN" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\x7fELF' or d[4] != 2:
    print(""); raise SystemExit
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
out = ""
for i in range(phnum):
    off = phoff + i * phentsize
    ptype, = struct.unpack_from('<I', d, off)
    if ptype != 2:                                  # PT_DYNAMIC
        continue
    poff, = struct.unpack_from('<Q', d, off + 0x08)
    psz,  = struct.unpack_from('<Q', d, off + 0x20)
    # strtab is found through DT_STRTAB (vaddr) -> file offset via PT_LOAD
    loads = []
    for k in range(phnum):
        o = phoff + k * phentsize
        t, = struct.unpack_from('<I', d, o)
        if t == 1:
            off_, va, fsz = (struct.unpack_from('<Q', d, o + 0x08)[0],
                             struct.unpack_from('<Q', d, o + 0x10)[0],
                             struct.unpack_from('<Q', d, o + 0x20)[0])
            loads.append((va, off_, fsz))
    def to_off(va):
        for base, o, sz in loads:
            if base <= va < base + sz:
                return o + (va - base)
        return None
    strtab = None; entries = []
    j = poff
    while j < poff + psz:
        tag, val = struct.unpack_from('<qQ', d, j)
        if tag == 0: break
        if tag == 5: strtab = val                   # DT_STRTAB
        if tag in (15, 29): entries.append((tag, val))
        j += 16
    if strtab is None: break
    so = to_off(strtab)
    for tag, val in entries:
        if tag != 15: continue                      # DT_RPATH only
        s = d.index(b'\0', so + val)
        out = d[so + val:s].decode()
print(out)
PY
)"
echo "DT_RPATH: $DT_RPATH"
[[ -n "$DT_RPATH" ]] || { echo "FAIL: executable carries no DT_RPATH"; exit 1; }

# The farm must be the last ABSOLUTE entry — not literally the last entry.
#
# `$ORIGIN`-relative entries are a different kind: they address the artifact's
# own directory, not this machine, so they travel with it and their position
# says nothing about which machine-local directory wins. A project with a shared
# library dependency gets one appended after everything else, and an assertion
# of "literally last" would fail on every such project while the invariant it
# meant to check still held. (Measured on a real GLFW app, whose DT_RPATH ends
# `… : <subos>/lib : $ORIGIN`.)
RPATH_LAST_ABS="$(python3 -c "
p = [x for x in '''$DT_RPATH'''.split(':') if x.startswith('/')]
print(p[-1] if p else '')
")"
[[ "$RPATH_LAST_ABS" == "$FARM" ]] || {
    echo "FAIL: the farm is not the last absolute entry of DT_RPATH"
    echo "      recorded farm:      $FARM"
    echo "      last absolute entry: $RPATH_LAST_ABS"
    echo "      full:               $DT_RPATH"
    exit 1
}

# ── invariant 3: libc still comes from the payload, not the farm ────────────
#
# The point of the ordering. Both directories can hold a libc.so.6 (the farm's
# is a symlink to the payload's today), so "it works" proves nothing — what
# matters is which one is consulted FIRST. Asserting the payload entry that
# precedes the farm actually holds a libc is what makes this a measurement.
RPATH_FIRST="${DT_RPATH%%:*}"
[[ -e "$RPATH_FIRST/libc.so.6" ]] || {
    echo "FAIL: the first DT_RPATH entry holds no libc.so.6, so this test is not"
    echo "      measuring the ordering it claims to measure."
    echo "      first: $RPATH_FIRST"
    exit 1
}
case ":$DT_RPATH:" in
    *":$FARM:"*) ;;
    *) echo "FAIL: farm missing from DT_RPATH entirely"; exit 1 ;;
esac
FARM_POS="$(python3 -c "
import sys
p = '''$DT_RPATH'''.split(':')
print(p.index('''$FARM'''))
")"
FIRST_POS=0
[[ "$FARM_POS" -gt "$FIRST_POS" ]] || {
    echo "FAIL: the farm precedes the payload that supplies libc"
    exit 1
}

echo "PASS: search closure is payload-first / farm-last, and the artifact agrees"
