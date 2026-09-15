#!/usr/bin/env bash
# requires: pack python3
# 722 -- `mcpp pack` strips what the graph built on every row that strips, and
# says so only when it does (#649 E5).
#
# `strip_program` used to strip the staged PROGRAM and nothing else, on the ELF
# and PE paths only. A shared library the same graph compiled from source
# shipped its `.symtab` and `.debug_*`, and the Android row, which `run`
# dispatches before either path, stripped nothing at all while the `Packing`
# line said "stripped". A member that stages libraries of its own had no way to
# learn the decision, so `--no-strip` did not reach its files.
#
# Legs:
#   A. Linux desktop, `bin` over a shared dependency: `bin/hostapp` and
#      `lib/libdep.so` carry no `.symtab` and no `.debug_*`; `libdep.so` still
#      exports `dep_answer` in `.dynsym`; the unpacked program runs.
#   B. `--no-strip`: `lib/libdep.so` keeps its `.symtab`, and the `Packing`
#      line does not say "stripped".
#   C. `--debug-symbols DIR`: `DIR/libdep.so.debug` exists and the packed
#      library names it through `.gnu_debuglink`.
#   D. A dispatched format's build program reads `pack_strip()` = "1" and
#      `pack_debug_symbols_dir()` = DIR, and "0" under `--no-strip`.
#   E. When the NDK is installed: `--target x86_64-linux-android` gives
#      `lib/libapp.so`, `lib/libdep.so` and `lib/libc++_shared.so` with no
#      `.symtab`, and `.dynsym` still names `app_entry` and `dep_answer`.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# Sections and dynamic symbols, read from the ELF itself.
elf_facts() {  # elf_facts <file>  ->  "symtab=N debug=N dynsym=<names,...>"
python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'\x7fELF' and d[4] == 2, "not ELF64"
shoff, = struct.unpack_from('<Q', d, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3A)
secs = []
for i in range(shnum):
    o = shoff + i * shentsize
    name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('<IIQQQQIIQQ', d, o)
    secs.append((name, typ, off, size, link, entsize))
stroff = secs[shstrndx][2]
def nm(n):
    e = d.index(b'\0', stroff + n)
    return d[stroff + n:e].decode()
names = [nm(s[0]) for s in secs]
symtab = sum(1 for n in names if n == '.symtab')
debug = sum(1 for n in names if n.startswith('.debug_'))
dyn = []
for (n, typ, off, size, link, entsize), sname in zip(secs, names):
    if sname != '.dynsym': continue
    stro = secs[link][2]
    for k in range(size // entsize):
        st_name, = struct.unpack_from('<I', d, off + k * entsize)
        if st_name:
            e = d.index(b'\0', stro + st_name)
            dyn.append(d[stro + st_name:e].decode())
link = 1 if '.gnu_debuglink' in names else 0
print(f"symtab={symtab} debug={debug} debuglink={link} dynsym={','.join(sorted(set(dyn)))}")
PY
}

# ── the fixture ─────────────────────────────────────────────────────────────
mkdir -p dep/src hostapp/src app/src
cat > dep/mcpp.toml <<'EOF'
[package]
name      = "dep"
namespace = "spike"
version   = "0.1.0"
standard  = "c++20"

[targets.dep]
kind = "shared"

[build]
sources = ["src/*.cpp"]
EOF
cat > dep/src/dep.cpp <<'EOF'
#include <string>
[[gnu::visibility("default")]] int dep_answer() { return static_cast<int>(std::to_string(42).size()) + 40; }
EOF

cat > hostapp/mcpp.toml <<'EOF'
[package]
name     = "hostapp"
version  = "0.1.0"
standard = "c++20"

[dependencies]
spike.dep = { path = "../dep" }

[targets.hostapp]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int dep_answer();\nint main() { return dep_answer() == 42 ? 0 : 1; }\n' > hostapp/src/main.cpp
cd hostapp

# ── A ───────────────────────────────────────────────────────────────────────
"$MCPP" pack --format tar > a.log 2>&1 || fail "A: pack failed" a.log
grep -q "Packing hostapp v0.1.0 (vendored, stripped)" a.log || fail "A: the Packing line" a.log
rm -rf xa && mkdir xa && tar -xzf target/dist/hostapp-0.1.0-x86_64-linux-gnu.tar.gz -C xa
lib=$(ls xa/*/lib/libdep.so); prog=$(ls xa/*/bin/hostapp)
fa=$(elf_facts "$lib"); fp=$(elf_facts "$prog")
echo "reading A: libdep.so $fa"
echo "reading A: hostapp   $fp"
case "$fa" in "symtab=0 debug=0 "*) ;; *) fail "A: lib/libdep.so is not stripped: $fa" ;; esac
case "$fa" in *dep_answer*) ;; *) fail "A: lib/libdep.so lost its export: $fa" ;; esac
case "$fp" in "symtab=0 debug=0 "*) ;; *) fail "A: bin/hostapp is not stripped: $fp" ;; esac
"$(ls -d xa/*)/hostapp" || fail "A: the unpacked program does not run" a.log
echo "ok: A, the program and the graph-built library are stripped and run"

# ── B ───────────────────────────────────────────────────────────────────────
"$MCPP" pack --format tar --no-strip > b.log 2>&1 || fail "B: pack failed" b.log
grep -q "Packing hostapp v0.1.0 (vendored)" b.log || fail "B: the Packing line" b.log
rm -rf xb && mkdir xb && tar -xzf target/dist/hostapp-0.1.0-x86_64-linux-gnu.tar.gz -C xb
fb=$(elf_facts "$(ls xb/*/lib/libdep.so)")
echo "reading B: libdep.so $fb"
case "$fb" in "symtab=1 "*) ;; *) fail "B: --no-strip did not reach lib/libdep.so: $fb" ;; esac
echo "ok: B, --no-strip reaches the graph-built library"

# ── C ───────────────────────────────────────────────────────────────────────
"$MCPP" pack --format tar --debug-symbols "$TMP/dbg" > c.log 2>&1 || fail "C: pack failed" c.log
[ -s "$TMP/dbg/libdep.so.debug" ] || fail "C: no libdep.so.debug" c.log
rm -rf xc && mkdir xc && tar -xzf target/dist/hostapp-0.1.0-x86_64-linux-gnu.tar.gz -C xc
fc=$(elf_facts "$(ls xc/*/lib/libdep.so)")
case "$fc" in *"debuglink=1"*) ;; *) fail "C: lib/libdep.so names no debug file: $fc" ;; esac
echo "ok: C, --debug-symbols separates the graph-built library's debug information"

# ── D ───────────────────────────────────────────────────────────────────────
# Written only now, so the legs above measure the strip itself on any engine.
# D's provider: records what a dispatched format's program is told.
cat > build.mcpp <<'EOF'
import mcpp;
#include <cstdio>
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("probe");
    if (std::string_view(mcpp::pack_format()) != "probe") return 0;
    const std::string out = std::string(mcpp::out_dir()) + "/probe.txt";
    const std::string text = std::string("strip=") + mcpp::pack_strip()
                           + " debug=" + mcpp::pack_debug_symbols_dir();
    mcpp::action a;
    a.id = "probe";
    a.role = "artifact";
    a.description = "probe";
    a.arg("/bin/sh").arg("-c").arg(("printf '%s' '" + text + "' > " + out).c_str())
     .input("${mcpp.target_file:hostapp}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF

"$MCPP" pack --format probe --debug-symbols "$TMP/dbg2" > d.log 2>&1 || fail "D: pack failed" d.log
probe=$(find target -name probe.txt | head -1)
[ "$(cat "$probe")" = "strip=1 debug=$TMP/dbg2" ] || fail "D: the program read '$(cat "$probe" 2>/dev/null)'" d.log
"$MCPP" pack --format probe --no-strip > d2.log 2>&1 || fail "D: pack --no-strip failed" d2.log
[ "$(cat "$probe")" = "strip=0 debug=" ] || fail "D: under --no-strip the program read '$(cat "$probe")'" d2.log
echo "ok: D, a dispatched format's program reads the strip decision"

# ── E ───────────────────────────────────────────────────────────────────────
ndk=$(ls -d "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-android-ndk/*/ 2>/dev/null | head -1 || true)
if [ -z "$ndk" ]; then
    echo "skip: E, xim:android-ndk is not installed on this machine"
else
    cd "$TMP"
    cat > app/mcpp.toml <<'EOF'
[package]
name     = "app"
version  = "0.1.0"
standard = "c++20"

[dependencies]
spike.dep = { path = "../dep" }

[targets.app]
kind = "app"
main = "src/main.cpp"

[target.x86_64-linux-android]
min_api_level = 23
EOF
    cat > app/src/main.cpp <<'EOF'
#include <string>
int dep_answer();
[[gnu::visibility("default")]] int app_entry() { return dep_answer() + static_cast<int>(std::to_string(1).size()); }
int main() { return app_entry() == 43 ? 0 : 1; }
EOF
    cd app
    "$MCPP" pack --target x86_64-linux-android --format tar > e.log 2>&1 || fail "E: pack failed" e.log
    grep -q "Packing app v0.1.0 (vendored, stripped)" e.log || fail "E: the Packing line" e.log
    rm -rf xe && mkdir xe && tar -xzf target/dist/app-0.1.0-x86_64-linux-android.tar.gz -C xe
    for f in libapp.so libdep.so libc++_shared.so; do
        fe=$(elf_facts "$(ls xe/*/lib/$f)")
        echo "reading E: $f $(echo "$fe" | cut -d' ' -f1-3)"
        case "$fe" in "symtab=0 debug=0 "*) ;; *) fail "E: lib/$f is not stripped: $fe" e.log ;; esac
    done
    case "$(elf_facts "$(ls xe/*/lib/libapp.so)")" in *app_entry*) ;; *) fail "E: libapp.so lost app_entry" ;; esac
    case "$(elf_facts "$(ls xe/*/lib/libdep.so)")" in *dep_answer*) ;; *) fail "E: libdep.so lost dep_answer" ;; esac
    echo "ok: E, the Android row strips the program, the graph's library and the NDK runtime"
fi

echo "PASS: 722"
