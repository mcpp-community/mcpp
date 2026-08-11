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
#   payload directories first, the artifact's own directory next,
#   the SubOS farm LAST — literally last, `$ORIGIN` included
#
# and it is about mutability, not taste. `<subos>/lib` is a symlink view
# rewritten by every `xlings install`; a payload directory is written once and
# the artifact's own directory holds the exact files this link resolved
# against. Payload-first keeps libc / libm / libstdc++ resolving from the
# pinned payload; `$ORIGIN` before the farm keeps the artifact running against
# what it was built with; the farm supplies only what nothing else does.
#
# Farm-first lets a later install silently change which library an ALREADY
# LINKED artifact loads — a failure that appears long after the build that
# caused it, and one that actually shipped: with the farm ahead of `$ORIGIN` a
# GLFW/imgui application linked against the libX11 mcpp had just built from
# `compat.x11` sources and then LOADED the `xim:libX11` xlings had installed.
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

# THE PROJECT CONSUMES A SHARED LIBRARY FROM A DEPENDENCY, AND THAT IS THE POINT.
#
# A bare `int main()` produces an executable with NO `$ORIGIN` in its DT_RPATH,
# so the ordering this test exists to check is not even present — the earlier
# version of this test passed on a binary that could not exhibit the bug.
#
# A shared TARGET in the same package is not enough either: mcpp links that
# package's module objects into the executable directly, so there is still no
# `-l` and no `$ORIGIN`. It takes a DEPENDENCY that ships a shared library —
# which is exactly the shape of the artifact that broke (an application whose
# `compat.x11` dependency builds `libX11.so` into the artifact directory).
mkdir -p "$TMP/greetdep/src" "$TMP/proj/src"
cat > "$TMP/greetdep/mcpp.toml" <<'EOF'
[package]
name = "greetdep"
version = "0.1.0"

[targets.greetdep]
kind = "shared"
EOF
# Interface and implementation are split so the call is a real cross-library
# reference: an inline definition in the interface would be emitted into the
# consumer and the dependency edge would vanish.
cat > "$TMP/greetdep/src/greetdep.cppm" <<'EOF'
export module greetdep;
export int greet_value();
EOF
cat > "$TMP/greetdep/src/greetdep.cpp" <<'EOF'
module greetdep;
int greet_value() { return 7; }
EOF

cd "$TMP/proj"
cat > mcpp.toml <<'EOF'
[package]
name = "closure"
version = "0.1.0"

[dependencies.greetdep]
path = "../greetdep"
EOF
cat > src/main.cpp <<'EOF'
import greetdep;
int main() { return greet_value() == 7 ? 0 : 1; }
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

# LITERALLY last, `$ORIGIN` included.
#
# An earlier version of this test filtered `$ORIGIN` out and asserted "the last
# ABSOLUTE entry", on the reasoning that an artifact-relative entry travels with
# the artifact and so "says nothing about which machine-local directory wins".
# That reasoning is backwards, and the filtered assertion is blind to the exact
# defect it was named after: with `… : <farm> : $ORIGIN` and with
# `… : $ORIGIN : <farm>` the list of absolute entries is IDENTICAL, so it passed
# on both. It reported "farm is last" on a binary whose farm was not last.
#
# What decides the winner is a SONAME present in two directories, and that is
# routine here: mcpp builds `compat.x11` from source into the artifact's own
# directory while xlings has `xim:libX11` in the farm. Farm-first meant an
# application linked against one libX11 and loaded the other, dying before main
# with `undefined symbol: _ZNKSt13runtime_error4whatEv`.
RPATH_LAST="$(python3 -c "
print('''$DT_RPATH'''.split(':')[-1])
")"
[[ "$RPATH_LAST" == "$FARM" ]] || {
    echo "FAIL: the farm is not the last entry of DT_RPATH"
    echo "      recorded farm: $FARM"
    echo "      last entry:    $RPATH_LAST"
    echo "      full:          $DT_RPATH"
    exit 1
}

# ── invariant 2b: the artifact's own directory is ON the path, and ahead ─────
#
# Both halves are load-bearing. Without `$ORIGIN` the project under test cannot
# exhibit the bug and every other assertion here is vacuous; with `$ORIGIN`
# behind the farm, a mutable view outranks the exact files this link resolved
# against.
case ":$DT_RPATH:" in
    *':$ORIGIN:'*) ;;
    *) echo "FAIL: no \$ORIGIN in DT_RPATH — this project cannot exercise the"
       echo "      ordering it is meant to check. full: $DT_RPATH"
       exit 1 ;;
esac
python3 - <<PY || exit 1
import sys
p = '''$DT_RPATH'''.split(':')
if p.index('\$ORIGIN') > p.index('''$FARM'''):
    print("FAIL: the SubOS farm outranks \$ORIGIN, so this artifact can load a")
    print("      different build of a library than it was linked against.")
    print("      full: " + ':'.join(p))
    sys.exit(1)
PY

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

# ── invariant 4: the LOADER agrees, measured rather than inferred ───────────
#
# Everything above reads a data structure. This runs the program and watches
# the dynamic linker walk the path, because the shape of DT_RPATH is a proxy
# and the behaviour is the thing: which physical file does the process open?
#
# THIS ASSERTION MUST NOT DEPEND ON A CRASH. The defect it guards produced a
# spectacular one (`undefined symbol: _ZNKSt13runtime_error4whatEv`), but that
# symptom exists only while shared libraries statically embed libstdc++. Once
# they stop, farm-first degrades from a crash to a SILENT version mismatch —
# the artifact quietly running a different build than it linked against — and
# an assertion written against the crash would go green for the wrong reason.
# So it asserts the search itself.
#
# `libgreetdep.so` lives in the artifact's directory and nowhere else, so if
# `$ORIGIN` is consulted first the farm is never tried for it at all. A farm
# path appearing in this trace means the farm was consulted FIRST.
LD_DEBUG=libs "$BIN" > "$TMP/run.log" 2> "$TMP/ld.log" || {
    echo "FAIL: the built executable did not run"; tail -20 "$TMP/ld.log"; exit 1;
}
SONAME="$(ls "$(dirname "$BIN")" | grep -E '^libgreetdep\.so' | head -1)"
[[ -n "$SONAME" ]] || {
    echo "FAIL: no shared library was produced, so invariant 4 is vacuous"
    ls -la "$(dirname "$BIN")"; exit 1;
}
awk -v soname="$SONAME" -v farm="$FARM" '
    index($0, "find library=" soname) { inblock = 1; next }
    inblock && /find library=/        { inblock = 0 }
    inblock && index($0, "trying file=") {
        if (index($0, farm "/")) { print; found = 1 }
    }
    END { exit found ? 1 : 0 }
' "$TMP/ld.log" || {
    echo "FAIL: the loader consulted the SubOS farm for $SONAME before the"
    echo "      artifact's own directory. A mutable view is being searched"
    echo "      ahead of the exact files this artifact was linked against."
    echo "      farm: $FARM"
    grep -A8 "find library=$SONAME" "$TMP/ld.log" | head -20
    exit 1
}

echo "PASS: search closure is payload-first / farm-last, the artifact agrees,"
echo "      and the loader resolves the artifact's own library from \$ORIGIN"
