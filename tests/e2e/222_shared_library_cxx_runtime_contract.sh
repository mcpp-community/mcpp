#!/usr/bin/env bash
# requires: gcc elf python3
# 222_shared_library_cxx_runtime_contract.sh — a shared library must not become
# the executable's C++ runtime.
#
# WHAT WENT WRONG
#
# `LinkUnit::SharedLibrary` shared the `Distributable` role with executables, so
# a .so got the same self-contained contract: `-static-libstdc++`. On ELF that
# is not a private copy. There is ONE global symbol namespace, a shared object
# exports every global it defines, and the embedded libstdc++ went into its
# dynamic symbol table UNVERSIONED — 777 GLOBAL std definitions (plus 2154 weak
# ones) out of a pure-C compat package. `libXau.so` was 9.5MB: 39KB of Xau and
# the rest libstdc++.
#
# The executable then linked `-lX11` BEFORE the driver's `-lstdc++`, so ld
# resolved its own `std::runtime_error::what()` against the .so and never
# pulled the archive member. Its `-static-libstdc++` became a no-op and its C++
# runtime was, in fact, that .so. Swap the .so for another build of the same
# SONAME — which is exactly what a farm-first DT_RPATH did — and the program
# dies before main:
#
#   undefined symbol: _ZNKSt13runtime_error4whatEv
#
# WHY THIS IS A SEPARATE TEST FROM 219
#
# 219 asserts the SEARCH ORDER. This asserts that a wrong search order can no
# longer be fatal, and that the executable keeps the contract it was promised.
# They fail independently and neither subsumes the other.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ── ELF readers: no binutils ────────────────────────────────────────────────
# A sandbox home does not reliably have binutils, and on at least one real
# machine its `readelf` shim pointed into a deleted directory.
cat > "$TMP/elf.py" <<'PY'
import struct, sys

def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2:
        raise SystemExit("not an ELF64 file: " + path)
    return d

def sections(d):
    # Elf64_Shdr: name(4) type(4) flags(8) addr(8) offset(8) size(8) link(4) …
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', d, 0x3A)
    for i in range(shnum):
        o = shoff + i * shentsize
        stype,  = struct.unpack_from('<I', d, o + 0x04)
        offset, = struct.unpack_from('<Q', d, o + 0x18)
        size,   = struct.unpack_from('<Q', d, o + 0x20)
        link,   = struct.unpack_from('<I', d, o + 0x28)
        yield {'type': stype, 'offset': offset, 'size': size, 'link': link}

def dynsyms(path, want_defined):
    """(name, binding) for dynamic symbols, defined or undefined.

    binding is STB_*: 1 = GLOBAL, 2 = WEAK. The distinction is the whole
    measurement — see `std-exports` below.

    Refuses an object with no SHT_DYNSYM rather than reporting an empty set:
    "exports nothing" and "was not measured" are the same number, and the whole
    point of this test is a count being zero.
    """
    d = load(path)
    secs = list(sections(d))
    found = False
    out = []
    for s in secs:
        if s['type'] != 11:                       # SHT_DYNSYM
            continue
        found = True
        strtab = secs[s['link']]
        for off in range(s['offset'], s['offset'] + s['size'], 24):
            st_name,  = struct.unpack_from('<I', d, off)
            st_info,  = struct.unpack_from('<B', d, off + 4)
            st_shndx, = struct.unpack_from('<H', d, off + 6)
            defined = st_shndx != 0                # SHN_UNDEF
            if defined != want_defined or st_name == 0:
                continue
            base = strtab['offset'] + st_name
            end = d.index(b'\0', base)
            n = d[base:end].decode('utf-8', 'replace')
            if n:
                out.append((n, st_info >> 4))
    if not found:
        raise SystemExit(
            "no .dynsym in " + path + " — this object was stripped of section "
            "headers, so a symbol count here would be zero for the wrong reason")
    return out

def dynsym_defined(path):   return dynsyms(path, True)
def dynsym_undefined(path): return dynsyms(path, False)

def needed(path):
    d = load(path)
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    loads, dyn = [], None
    for i in range(phnum):
        o = phoff + i * phentsize
        t, = struct.unpack_from('<I', d, o)
        off_, va, fsz = (struct.unpack_from('<Q', d, o + 0x08)[0],
                         struct.unpack_from('<Q', d, o + 0x10)[0],
                         struct.unpack_from('<Q', d, o + 0x20)[0])
        if t == 1: loads.append((va, off_, fsz))
        if t == 2: dyn = (off_, fsz)
    if dyn is None: return []
    def to_off(va):
        for base, o, sz in loads:
            if base <= va < base + sz:
                return o + (va - base)
        return None
    strtab, entries = None, []
    j = dyn[0]
    while j < dyn[0] + dyn[1]:
        tag, val = struct.unpack_from('<qQ', d, j)
        if tag == 0: break
        if tag == 5: strtab = val
        if tag == 1: entries.append(val)
        j += 16
    if strtab is None: return []
    so = to_off(strtab)
    out = []
    for val in entries:
        end = d.index(b'\0', so + val)
        out.append(d[so + val:end].decode())
    return out

STD = ('_ZNSt', '_ZNKSt', '_ZSt', '_ZTVSt', '_ZTISt')

if __name__ == '__main__':
    what, path = sys.argv[1], sys.argv[2]
    if what == 'std-exports':
        # GLOBAL ONLY, and that is the measurement, not a loosening.
        #
        # A C++ shared library that uses std::string legitimately exports
        # ~30 std symbols — template instantiations emitted from headers into
        # its own translation units. They are WEAK/COMDAT by construction, and
        # the loader unifying them across the process is the intended C++ ABI
        # behaviour, not a leak.
        #
        # What must never appear is a GLOBAL std definition: those come from
        # libstdc++.a and only from there. `_ZNKSt13runtime_error4whatEv` is a
        # GLOBAL `T`; the broken libXau.so exported 777 of them next to 2777
        # weak ones. Counting both would make this assertion unsatisfiable and
        # it would have to be deleted — which is how a real invariant gets
        # traded for no invariant at all.
        print(sum(1 for n, b in dynsym_defined(path)
                  if n.startswith(STD) and b == 1))
    elif what == 'std-undefined':
        print(sum(1 for n, _ in dynsym_undefined(path) if n.startswith(STD)))
    elif what == 'defines':
        sym = sys.argv[3]
        print('yes' if any(n == sym for n, _ in dynsym_defined(path)) else 'no')
    elif what == 'needed':
        print('\n'.join(needed(path)))
PY

elf() { python3 "$TMP/elf.py" "$@"; }

# The exact symbol whose absence killed the original artifact. A count can be
# argued about; this one cannot — it is a GLOBAL definition that exists only
# inside libstdc++.a, so a shared library exporting it has embedded the runtime
# and is about to become somebody else's.
CRASH_SYM=_ZNKSt13runtime_error4whatEv

# ── a project with a C++ shared library and an executable that uses it ──────
mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > mcpp.toml <<'EOF'
[package]
name = "sharedrt"
version = "0.1.0"

[targets.sharedrtlib]
kind = "shared"

[targets.sharedrt]
kind = "bin"
main = "src/main.cpp"
EOF
# The library genuinely uses the standard library, and the executable
# genuinely throws — `std::runtime_error::what()` is the exact symbol whose
# disappearance produced the original crash.
cat > src/lib.cppm <<'EOF'
export module sharedrt.lib;
import std;
export std::string sharedrt_greet() {
    try { throw std::runtime_error("hello"); }
    catch (const std::exception& e) { return std::string(e.what()); }
}
EOF
cat > src/main.cpp <<'EOF'
import std;
import sharedrt.lib;
int main() { return sharedrt_greet() == "hello" ? 0 : 1; }
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

BIN="$(ls target/*/*/bin/sharedrt 2>/dev/null | head -1)"
LIB="$(ls target/*/*/bin/libsharedrtlib.so 2>/dev/null | head -1)"
[[ -n "$BIN" ]] || { echo "FAIL: no executable"; ls -R target | head -40; exit 1; }
[[ -n "$LIB" ]] || { echo "FAIL: no shared library"; ls -R target | head -40; exit 1; }

# ── invariant 1: the shared library exports no libstdc++ RUNTIME symbols ────
#
# Zero GLOBAL std definitions, not "few". One is enough to satisfy an
# executable's reference and take over its runtime — that is not a matter of
# degree. (Weak/COMDAT template instantiations are excluded and must be: a
# library using std::string emits ~30 of them and unifying those across the
# process is the intended C++ ABI behaviour. See `std-exports` in elf.py.)
EXPORTED="$(elf std-exports "$LIB")"
[[ "$EXPORTED" == "0" ]] || {
    echo "FAIL: the shared library exports $EXPORTED GLOBAL standard-library"
    echo "      symbols. Those come from libstdc++.a and nowhere else, so an"
    echo "      executable linking this library will bind ITS std references"
    echo "      here and silently lose its own C++ runtime contract."
    exit 1
}
[[ "$(elf defines "$LIB" "$CRASH_SYM")" == "no" ]] || {
    echo "FAIL: the shared library exports $CRASH_SYM — the exact symbol whose"
    echo "      disappearance killed the artifact this test exists for."
    exit 1
}

# ── invariant 2: …because it COUPLES to the runtime instead of embedding it ─
#
# Asserted separately from invariant 1 on purpose. "Exports nothing" would also
# be true of a library that embedded a HIDDEN copy, and that is a different
# artifact with a different failure mode (two std runtimes in one process).
# What the default promises on ELF is one runtime, shared.
elf needed "$LIB" | grep -qx 'libstdc++.so.6' || {
    echo "FAIL: the shared library declares no dependency on libstdc++.so.6, so"
    echo "      it is carrying a private C++ runtime after all. NEEDED was:"
    elf needed "$LIB"
    exit 1
}

# ── invariant 3: the EXECUTABLE keeps its own contract ──────────────────────
#
# This is the payoff, and the exact shape of the original crash: with the .so
# exporting std, the executable's `-static-libstdc++` silently became a no-op
# and it carried an UNDEFINED `_ZNKSt13runtime_error4whatEv` that only the .so
# could satisfy.
UNDEF="$(elf std-undefined "$BIN")"
[[ "$UNDEF" == "0" ]] || {
    echo "FAIL: the executable has $UNDEF undefined standard-library symbols."
    echo "      Its self-contained C++ runtime is being supplied by something else."
    exit 1
}

"$BIN" || { echo "FAIL: the executable did not run"; exit 1; }

# ── invariant 4: the escape hatch works AND stays guarded ───────────────────
#
# `cxx_runtime = { shared = "self-contained" }` is legitimate — a .so that
# ships alone needs it. What must not come back is the export. `--exclude-libs`
# is what keeps the hatch from re-opening the defect.
cat > mcpp.toml <<'EOF'
[package]
name = "sharedrt"
version = "0.1.0"

[build]
cxx_runtime = { shared = "self-contained" }

[targets.sharedrtlib]
kind = "shared"

[targets.sharedrt]
kind = "bin"
main = "src/main.cpp"
EOF
rm -rf target
"$MCPP" build > build2.log 2>&1 || { cat build2.log; exit 1; }
LIB="$(ls target/*/*/bin/libsharedrtlib.so 2>/dev/null | head -1)"
[[ -n "$LIB" ]] || { echo "FAIL: no shared library on the second build"; exit 1; }

elf needed "$LIB" | grep -qx 'libstdc++.so.6' && {
    echo "FAIL: cxx_runtime = { shared = \"self-contained\" } was ignored —"
    echo "      the library still couples to libstdc++.so.6."
    exit 1
}
EXPORTED="$(elf std-exports "$LIB")"
[[ "$EXPORTED" == "0" ]] || {
    echo "FAIL: an explicitly self-contained shared library exported $EXPORTED"
    echo "      GLOBAL standard-library symbols. --exclude-libs is missing, so"
    echo "      the escape hatch re-opens the defect it was allowed to work"
    echo "      around."
    exit 1
}
[[ "$(elf defines "$LIB" "$CRASH_SYM")" == "no" ]] || {
    echo "FAIL: the embedded runtime is exported — $CRASH_SYM is visible from"
    echo "      an explicitly self-contained shared library."
    exit 1
}

echo "PASS: a shared library couples to the C++ runtime instead of exporting it,"
echo "      the executable keeps its own contract, and the opt-out stays hidden"
