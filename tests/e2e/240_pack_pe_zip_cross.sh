#!/usr/bin/env bash
# requires: mingw-cross python3
# 240_pack_pe_zip_cross.sh — packaging a Windows program FROM LINUX.
#
# This is the acceptance criterion for §4 of
# .agents/docs/2026-08-16-windows-toolchain-three-axes-design.md, and it is
# written so that running it on Windows would prove nothing:
#
#   "在 Linux 上为 Windows 产物打出 zip,内含正确 DLL 闭包
#    — 跨 OS 是这条的全部意义;同 OS 打包证明不了"
#
# `mcpp pack` used to refuse Windows with `#if defined(_WIN32)`, and the
# reason given was that the tools were POSIX-only. The real reason was one
# layer down: the dependency closure came from
#
#     LD_TRACE_LOADED_OBJECTS=1 '<binary>'
#
# which RUNS the artifact — so it could cross neither an OS nor an
# architecture, and no amount of porting `tar` would have helped. Reading the
# import table instead (mcpp.pack.binfmt) removes both limits at once, and
# this test is what says that actually happened rather than that the guard was
# deleted.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new winpack > /dev/null
cd winpack
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("winpack ok\n"); return 0; }
EOF
rm -f src/winpack.cppm

cat > mcpp.toml <<'EOF'
[package]
name = "winpack"
version = "0.1.0"

# ⚠️ THIS TEST DROPS A FILE INTO THE BUILD TREE AND EXPECTS `pack` TO SEE IT,
# so the two commands have to agree on WHICH build tree that is.
#
# `mcpp pack` builds with the `release` fallback (a packaged artifact leaves
# this machine), while a bare `mcpp build` uses `dev` — two profiles, two
# fingerprint directories, and the stand-in DLL below would land in the one
# `pack` does not use. Nothing about the closure would be wrong; the file would
# simply not be there, and the assertion would read as "the closure reader
# failed".
#
# Stating the profile in the manifest settles it for both, and doubles as a
# check that `[build] default-profile` still outranks pack's fallback.
[build]
default-profile = "dev"

# `force_bundle` reaching a SYSTEM name is what makes the next assertion
# positive rather than vacuous — see the comment at the msvcrt.dll check.
[pack.bundle-project]
force_bundle = ["msvcrt.dll"]
EOF

"$MCPP" build --target x86_64-windows-gnu > build.log 2>&1 || { cat build.log; exit 1; }
EXE="$(find target/x86_64-windows-gnu -name 'winpack.exe' | head -1)"
[[ -n "$EXE" ]] || { echo "FAIL: no PE was produced"; exit 1; }

# The artifact really is a PE, checked here rather than assumed from the
# triple: everything below is about reading THAT file.
python3 - "$EXE" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:2] == b'MZ', "not an MZ image"
lfanew, = struct.unpack_from('<I', d, 0x3C)
assert d[lfanew:lfanew+4] == b'PE\0\0', "no PE signature"
machine, = struct.unpack_from('<H', d, lfanew+4)
assert machine == 0x8664, f"unexpected machine {machine:#x}"
PY

# A stand-in for the DLL the program imports. It is a plain file, not a real
# library: nothing here loads it, which is the whole point — the closure is
# derived from the EXE's import table, and a resolver that needed the
# dependency to be loadable would be back where it started.
BINDIR="$(dirname "$EXE")"
printf 'MZ-not-a-real-dll' > "$BINDIR/msvcrt.dll"

"$MCPP" pack --target x86_64-windows-gnu > pack.log 2>&1 || { cat pack.log; exit 1; }

ZIP="$(find target/dist -name '*.zip' | head -1)"
[[ -n "$ZIP" ]] || {
    echo "FAIL: no .zip produced — a Windows package is a zip whoever built it"
    ls -R target/dist || true
    cat pack.log
    exit 1
}
# And NOT a tarball: the extension follows the artifact, not the host.
[[ -z "$(find target/dist -name '*.tar.gz' | head -1)" ]] || {
    echo "FAIL: produced a .tar.gz for a Windows target"; exit 1; }

python3 - "$ZIP" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])

# An INDEPENDENT reader. mcpp writes this archive itself (no host has a zip
# tool that exists everywhere), so "our writer agrees with our reader" would
# be worth nothing.
bad = z.testzip()
assert bad is None, f"corrupt entry: {bad}"

names = z.namelist()
tops = {n.split('/')[0] for n in names}
assert len(tops) == 1, f"archive has no single wrapper directory: {tops}"
wrapper = tops.pop()

exe = f"{wrapper}/winpack.exe"
assert exe in names, f"the executable is missing: {names}"

# FLAT, beside the .exe. On PE that is not a layout preference — it is the
# relocation mechanism: the Win32 loader resolves a DLL from the directory of
# the executable, and there is no rpath to point anywhere else.
dll = f"{wrapper}/msvcrt.dll"
assert dll in names, (
    "msvcrt.dll was not bundled. It is named in the EXE's import table and in "
    f"[pack] force_bundle, so this is the closure reader failing: {names}")
assert z.read(dll) == b'MZ-not-a-real-dll', "bundled the wrong file"

# The other half, which the assertion above cannot give: the closure must
# EXCLUDE Windows' own. A parser that read nothing at all would also produce
# an archive with no kernel32.dll in it — but it could not have produced the
# msvcrt.dll above, so the two together are decisive.
lower = [n.lower() for n in names]
for sysdll in ("kernel32.dll", "ntdll.dll", "ucrtbase.dll"):
    assert not any(n.endswith(sysdll) for n in lower), (
        f"{sysdll} was bundled — shipping a private copy of a Windows "
        "component is a broken program, not a heavier one")

# The .exe keeps its executable bit, which only matters because a Windows
# package is routinely unpacked and inspected (and wine-tested) from Linux.
info = z.getinfo(exe)
assert (info.external_attr >> 16) & 0o111, "the executable bit was lost"
print("OK: zip verified by an independent reader")
PY

# `--mode system` promises the target provides everything. A DLL beside the
# .exe would contradict that, and the modes have to mean the same thing on
# both formats or the flag is decoration.
rm -rf target/dist
"$MCPP" pack --target x86_64-windows-gnu --mode system > pack2.log 2>&1 || {
    cat pack2.log; exit 1; }
python3 - "$(find target/dist -name '*.zip' | head -1)" <<'PY'
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
assert not any(n.lower().endswith('.dll') for n in names), \
    f"--mode system bundled a DLL: {names}"
PY

# The contract reaches packaging (design §4.3). `toolchain-coupled` says the
# toolchain's runtime travels WITH the artifact; a mode that bundles nothing
# cannot deliver that, and a contract with no executor is a promise the build
# prints and the package quietly drops.
cat >> mcpp.toml <<'EOF'

[build]
cxx_runtime = "toolchain-coupled"
EOF
rm -rf target/dist
if "$MCPP" pack --target x86_64-windows-gnu --mode system > pack3.log 2>&1; then
    echo "FAIL: cxx_runtime = toolchain-coupled + --mode system was accepted"
    cat pack3.log
    exit 1
fi
grep -q 'toolchain-coupled' pack3.log || {
    echo "FAIL: the refusal does not name the contract"; cat pack3.log; exit 1; }
grep -q 'vendored' pack3.log || {
    echo "FAIL: the refusal offers no way out"; cat pack3.log; exit 1; }

echo "OK"
