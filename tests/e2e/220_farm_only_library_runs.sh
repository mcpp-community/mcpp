#!/usr/bin/env bash
# requires: gcc elf python3
# 220_farm_only_library_runs.sh — the assertion that cannot be faked.
#
# TWO HALVES, BOTH ABOUT THE SAME MISTAKE
#
# 1. A library that only the SubOS farm provides must LOAD. Not "the path is in
#    DT_RPATH" — 219 asserts that, and it is not enough: the artifact runs under
#    a PRIVATE loader whose defaults differ from the host's, so a path can be
#    present, correct, and still not be what the program consults. Only exec
#    tells the truth.
#
#    This is also why the host cannot be used as an oracle. On a developer
#    machine /usr/lib usually has libGL.so.1 too, so a checker that falls back
#    to host directories reports "resolved" for a binary that exits 127. That
#    is precisely how `validation: pass` was printed for a program that could
#    not start.
#
# 2. A DT_NEEDED that NOTHING provides must turn the build RED. Under a
#    hermetic binding mcpp computes the whole search path, so "not found" is a
#    measurement, not an absence of one — reporting it as `inconclusive` files
#    a proven failure under "not checked".
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ── find a library only the farm provides ───────────────────────────────────
#
# Derived from the recorded closure rather than hardcoded to libGL: CI SubOSes
# do not all have graphics packages installed, and a test that names a library
# it cannot find would either fail for the wrong reason or quietly pass.
mkdir -p "$TMP/probe/src"
cd "$TMP/probe"
cat > mcpp.toml <<'EOF'
[package]
name = "probe"
version = "0.1.0"
EOF
echo 'int main() { return 0; }' > src/main.cpp
"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }
RES="$(ls target/*/*/resolution.json | head -1)"

read -r FARM_LIB FARM_DIR <<<"$(python3 - "$RES" <<'PY'
import json, os, sys, re
doc = json.load(open(sys.argv[1]))
closure = doc["runtime"].get("search", {}).get("closure", [])
farms    = [d["path"] for d in closure if d["origin"] == "subos_farm"]
payloads = [d["path"] for d in closure if d["origin"] != "subos_farm"]

# Names the payloads already provide are useless here: the farm would not be
# the reason they load.
provided = set()
for p in payloads:
    try: provided.update(os.listdir(p))
    except OSError: pass

# A link name (`libfoo.so`) whose SONAME-ish sibling exists, skipping the C
# runtime: those are the payload's job and are on the search path ahead of the
# farm by construction.
skip = re.compile(r'^lib(c|m|dl|rt|pthread|gcc_s|stdc\+\+|atomic|c\+\+.*)\.so')
for d in farms:
    try: names = sorted(os.listdir(d))
    except OSError: continue
    for n in names:
        if not n.endswith(".so") or not n.startswith("lib"): continue
        if skip.match(n): continue
        if n in provided: continue
        if not os.path.exists(os.path.join(d, n[:-3] + ".so.1")) \
           and not os.path.islink(os.path.join(d, n)): continue
        print(n[3:-3], d)
        raise SystemExit
print("", "")
PY
)"

if [[ -z "$FARM_LIB" ]]; then
    # SKIP, LOUDLY, WITH THE REASON. Silence here would mean "the farm works"
    # on a machine where nothing was ever loaded from it.
    echo "SKIP: this SubOS farm provides no library the payloads do not already"
    echo "      provide, so there is nothing whose loading proves the farm is"
    echo "      reachable. Install any library into the SubOS to exercise this."
    exit 0
fi
echo "farm-only library: -l$FARM_LIB  (from $FARM_DIR)"

# ── half 1: it must LINK and RUN ────────────────────────────────────────────
#
# `--no-as-needed` is what makes this a test of the loader rather than of the
# linker: with no symbol referenced, --as-needed would drop the DT_NEEDED
# entirely and the program would run for a reason that proves nothing.
mkdir -p "$TMP/uses/src"
cd "$TMP/uses"
cat > mcpp.toml <<EOF
[package]
name = "uses"
version = "0.1.0"

[build]
ldflags = ["-Wl,--no-as-needed", "-l$FARM_LIB", "-Wl,--as-needed"]
EOF
echo 'int main() { return 0; }' > src/main.cpp

"$MCPP" build > build.log 2>&1 || {
    echo "FAIL: linking against a farm-provided library failed"
    cat build.log
    exit 1
}
BIN="$(ls target/*/*/bin/uses | head -1)"
python3 - "$BIN" "$FARM_LIB" <<'PY' || exit 1
import struct, sys
d = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
loads, dyn = [], None
for i in range(phnum):
    o = phoff + i * phentsize
    t, = struct.unpack_from('<I', d, o)
    if t == 1:
        loads.append((struct.unpack_from('<Q', d, o + 0x10)[0],
                      struct.unpack_from('<Q', d, o + 0x08)[0],
                      struct.unpack_from('<Q', d, o + 0x20)[0]))
    if t == 2:
        dyn = (struct.unpack_from('<Q', d, o + 0x08)[0],
               struct.unpack_from('<Q', d, o + 0x20)[0])
def to_off(va):
    for base, off, sz in loads:
        if base <= va < base + sz: return off + (va - base)
poff, psz = dyn
strtab, needed = None, []
j = poff
while j < poff + psz:
    tag, val = struct.unpack_from('<qQ', d, j)
    if tag == 0: break
    if tag == 5: strtab = val
    if tag == 1: needed.append(val)
    j += 16
so = to_off(strtab)
names = [d[so+v:d.index(b'\0', so+v)].decode() for v in needed]
want = "lib" + sys.argv[2] + ".so"
if not any(n.startswith(want) for n in names):
    print(f"FAIL: {want} is not DT_NEEDED — --no-as-needed did not hold, so the"
          f" run below would prove nothing. NEEDED: {names}")
    raise SystemExit(1)
print("DT_NEEDED includes:", [n for n in names if n.startswith(want)])
PY

"$BIN"
rc=$?
[[ $rc -eq 0 ]] || {
    echo "FAIL: the artifact could not start (exit $rc)"
    echo "      Its DT_NEEDED is provided ONLY by the SubOS farm, so this is the"
    echo "      run-time half of the closure: the path is reachable at link time"
    echo "      (--sysroot=<subos>) and must be reachable at load time too."
    exit 1
}
echo "ran: exit 0 with a farm-only DT_NEEDED"

# ── half 2: an unsatisfiable DT_NEEDED must turn the build RED ──────────────
#
# Built by giving a real shared library a SONAME that names no file anywhere.
# The link succeeds (ld was handed the file), the DT_NEEDED records the SONAME,
# and nothing on the artifact's search path can ever satisfy it.
mkdir -p "$TMP/ghostlib/src"
cd "$TMP/ghostlib"
cat > mcpp.toml <<'EOF'
[package]
name = "ghostlib"
version = "0.1.0"

[targets.ghost]
kind = "shared"

[build]
ldflags = ["-Wl,-soname,libmcpp_ghost_probe.so.1"]
EOF
cat > src/ghost.cppm <<'EOF'
export module ghostlib.ghost;
export int ghost_value() { return 1; }
EOF
"$MCPP" build > build.log 2>&1 || { echo "FAIL: ghost library build"; cat build.log; exit 1; }
GHOST_SO="$(ls target/*/*/bin/libghost.so 2>/dev/null | head -1)"
[[ -n "$GHOST_SO" ]] || { echo "FAIL: no libghost.so produced"; ls -R target | head -30; exit 1; }
# ABSOLUTE. The consumer below is built from a different directory, and a
# relative -L there resolves to nothing — which fails the build for the wrong
# reason and would read exactly like the failure this half is asserting.
GHOST_DIR="$(cd "$(dirname "$GHOST_SO")" && pwd)"

mkdir -p "$TMP/needsghost/src"
cd "$TMP/needsghost"
cat > mcpp.toml <<EOF
[package]
name = "needsghost"
version = "0.1.0"

[build]
ldflags = ["-Wl,--no-as-needed", "-L$GHOST_DIR", "-lghost", "-Wl,--as-needed"]
EOF
echo 'int main() { return 0; }' > src/main.cpp

if "$MCPP" build > build.log 2>&1; then
    echo "FAIL: a DT_NEEDED that nothing can provide built successfully"
    echo "      The artifact cannot start; reporting that as a pass is the"
    echo "      defect this half exists for."
    grep -i "ghost" build.log | head
    exit 1
fi
grep -qi "libmcpp_ghost_probe.so.1" build.log || {
    echo "FAIL: the build failed, but not for the unresolvable DT_NEEDED —"
    echo "      a failure that does not name the cause is not this assertion."
    cat build.log
    exit 1
}
echo "unresolvable DT_NEEDED failed the build and named itself"

echo "PASS: farm-only libraries load, unsatisfiable ones fail the build"
