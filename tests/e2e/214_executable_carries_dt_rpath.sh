#!/usr/bin/env bash
# requires: gcc elf python3
# 214_executable_carries_dt_rpath.sh — the loader-tag contract.
#
#   executable      DT_RPATH
#   shared library  DT_RUNPATH
#
# and both halves are measured, not stylistic.
#
# DT_RUNPATH is consulted only for the object carrying it and for the dlopen()
# that object performs ITSELF. DT_RPATH is consulted for every dlopen anywhere
# in the process. A GL program reaches its driver through three to four
# dlopen() calls it does not make — libGLX.so.0 makes them — so an executable
# tagged DT_RUNPATH has the right path and cannot reach through it: same paths,
# tag flipped, egl/gles2/egl-surfaceless move from llvmpipe to the GPU.
#
# The other half runs the opposite way: forcing DT_RPATH onto a LIBRARY pushes
# its search path into every lookup below it and eglInitialize fails outright
# (openxlings/xlings#593). So this is a split, and both sides are asserted.
#
# WHY THIS TEST ASSERTS THE DEFAULT FIRST
#
# Every linker mcpp targets defaults to --enable-new-dtags today. If that ever
# changes, an assertion of "the executable has DT_RPATH" would keep passing
# while the flag that produces it had been deleted — a test that cannot fail is
# indistinguishable from one that is not running. So step 0 builds WITHOUT the
# contract and requires DT_RUNPATH: the test states its own premise.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ── tag reader: no external tool ────────────────────────────────────────────
# The binutils in a sandbox home is not reliably present (and on at least one
# real machine its shims pointed at a deleted directory), so the dynamic
# section is parsed directly. Reads the WHOLE section, not the first hit: with
# both tags present the loader ignores DT_RPATH, and DT_RPATH-first is the
# common layout, so a first-hit reader reports the opposite of the truth.
read_tag() {
python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\x7fELF' or d[4] != 2:
    print("NOT-ELF64"); raise SystemExit
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
rpath = runpath = interp = False
for i in range(phnum):
    off = phoff + i * phentsize
    ptype, = struct.unpack_from('<I', d, off)
    if ptype == 3:                        # PT_INTERP
        interp = True
    if ptype == 2:                        # PT_DYNAMIC
        poff, = struct.unpack_from('<Q', d, off + 0x08)
        psz,  = struct.unpack_from('<Q', d, off + 0x20)
        j = poff
        while j < poff + psz:
            tag, _ = struct.unpack_from('<qQ', d, j)
            if tag == 0: break
            if tag == 15: rpath = True    # DT_RPATH
            if tag == 29: runpath = True  # DT_RUNPATH
            j += 16
form = "executable" if interp else "shared_library"
tag = ("BOTH" if rpath and runpath else
       "RPATH" if rpath else "RUNPATH" if runpath else "NONE")
print(f"{form} {tag}")
PY
}

# ── a project with one bin and one shared library ───────────────────────────
mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > mcpp.toml <<'EOF'
[package]
name = "tags"
version = "0.1.0"

[targets.taglib]
kind = "shared"

[targets.tagbin]
kind = "bin"
main = "src/main.cpp"
EOF
cat > src/lib.cppm <<'EOF'
export module tags.lib;
export int tag_value() { return 7; }
EOF
cat > src/main.cpp <<'EOF'
import tags.lib;
int main() { return tag_value() == 7 ? 0 : 1; }
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

# ── step 0: the flag must be where the contract says, and only there ────────
#
# THE SHARED LIBRARY IS THE CONTROL GROUP. It is linked by the same driver,
# with the same ldflags, and deliberately WITHOUT the contract flag — so its
# tag is this linker's default. That makes the premise self-checking without a
# probe binary: if the default ever becomes DT_RPATH, the library assertion
# below fails and says so, instead of the executable assertion passing for a
# reason that has nothing to do with mcpp.
#
# Reading it out of the graph as well as off the binary matters because they
# fail differently: a missing flag is a regression in mcpp, while a flag that
# is present but produced the wrong tag is a regression in the toolchain.
NINJA="$(ls target/*/*/build.ninja | head -1)"
[[ -n "$NINJA" ]] || { echo "FAIL: no build.ninja"; exit 1; }

bin_edge="$(grep -A4 -E '^build bin/tagbin *: *cxx_link' "$NINJA" | grep unit_ldflags || true)"
lib_edge="$(grep -A4 -E '^build bin/libtaglib\.so.* : *cxx_shared' "$NINJA" | grep unit_ldflags || true)"
case "$bin_edge" in
    *--disable-new-dtags*) ;;
    *)
        echo "FAIL: the executable's link edge carries no loader-tag flag"
        echo "      $bin_edge"
        exit 1
        ;;
esac
case "$lib_edge" in
    *--disable-new-dtags*)
        echo "FAIL: the shared library's link edge carries the executable's flag"
        echo "      forcing DT_RPATH on a library breaks eglInitialize (xlings#593)"
        echo "      $lib_edge"
        exit 1
        ;;
esac

BIN="$(ls target/*/*/bin/tagbin 2>/dev/null | head -1)"
LIB="$(ls target/*/*/bin/libtaglib.so 2>/dev/null | head -1)"
[[ -n "$BIN" ]] || { echo "FAIL: no executable produced"; ls -R target | head -40; exit 1; }
[[ -n "$LIB" ]] || { echo "FAIL: no shared library produced"; ls -R target | head -40; exit 1; }

bin_tag="$(read_tag "$BIN")"
lib_tag="$(read_tag "$LIB")"
echo "executable: $bin_tag"
echo "library:    $lib_tag"

case "$bin_tag" in
    "executable RPATH") ;;
    *)
        echo "FAIL: executable must carry DT_RPATH, got: $bin_tag"
        echo "      a search path under DT_RUNPATH is unreachable from a dlopen()"
        echo "      performed by another object — which is every dlopen in the"
        echo "      graphics stack"
        grep -n 'unit_ldflags' target/*/*/build.ninja | head
        exit 1
        ;;
esac
case "$lib_tag" in
    "shared_library RUNPATH"|"shared_library NONE") ;;
    "shared_library RPATH"|"shared_library BOTH")
        echo "FAIL: shared library came out as $lib_tag"
        echo "      Its link edge carries NO loader-tag flag (asserted above), so"
        echo "      this is the linker's DEFAULT — and if the default is now"
        echo "      DT_RPATH, the executable assertion below is passing for a"
        echo "      reason unrelated to mcpp. Re-derive the contract."
        echo "      Also: forcing DT_RPATH on a library pushes its search path"
        echo "      into every lookup below it (openxlings/xlings#593)."
        exit 1
        ;;
    *)
        echo "FAIL: unexpected library tag: $lib_tag"
        exit 1
        ;;
esac

# ── the finding must be recorded, not just true ─────────────────────────────
# A check whose only output is silence is indistinguishable from a check that
# never ran. resolution.json is where the answer lives.
RES="$(ls target/*/*/resolution.json 2>/dev/null | head -1)"
[[ -n "$RES" ]] || { echo "FAIL: no resolution.json"; exit 1; }
grep -q '"loader_tags"' "$RES" || {
    echo "FAIL: resolution.json records no loader_tags — rule E did not run"
    exit 1
}
if grep -q '"status": *"violation"' "$RES"; then
    echo "FAIL: rule E reported a violation on mcpp's own output"
    python3 -c "import json,sys;print(json.dumps(json.load(open(sys.argv[1]))['runtime']['loader_tags'],indent=1))" "$RES"
    exit 1
fi
# `ok` for both artifacts, not merely "no violation": absence of a violation is
# also what a check that silently did nothing produces.
python3 - "$RES" <<'PY2' || exit 1
import json, sys
tags = json.load(open(sys.argv[1]))["runtime"]["loader_tags"]
by_form = {t["form"]: t for t in tags}
for form, want in (("executable", "DT_RPATH"), ("shared_library", "DT_RUNPATH")):
    t = by_form.get(form)
    if t is None:
        print(f"FAIL: rule E recorded no {form}"); raise SystemExit(1)
    if t["status"] != "ok" or t["required"] != want:
        print(f"FAIL: {form}: {t}"); raise SystemExit(1)
print("rule E: executable=%s library=%s" % (
    by_form["executable"]["actual"], by_form["shared_library"]["actual"]))
PY2

# and it has to run
"$MCPP" run tagbin > run.log 2>&1 || { cat run.log; exit 1; }

echo "PASS: executables carry DT_RPATH, shared libraries keep DT_RUNPATH, and rule E recorded it"
