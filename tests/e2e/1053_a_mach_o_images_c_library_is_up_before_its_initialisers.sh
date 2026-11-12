#!/usr/bin/env bash
# requires: llvm unix-shell
# 1053 -- a Mach-O image's C library is up before the program's initialisers
# (2026.11.9.1).
#
# The loader of this format runs an image's initialisers before its entry, in
# the order the linker placed their objects. A C library of the graph
# (`mcpp:c-abi`: openkal-musl, in openkal-platform's hosted arrangement) is
# brought up by an initialiser of its own; the program's objects came first on
# the link line, and a global constructor that allocated -- the registration of
# an mtest case -- stopped in an allocator not yet started (mcpplibs/os#22,
# hosted macOS, EXC_BAD_ACCESS in mimalloc). The C library's objects now lead
# the link, as a C runtime's start objects do on ELF.
#
# Criteria:
#   A. In the image built for `aarch64-macos-musl` over the hosted arrangement, the
#      C library's initialiser (`okm_early_init`, openkal-musl's
#      port/src/mach/early_init.c) precedes the program's own.
#   B. On an arm64 macOS host the program runs, its global constructor
#      allocating before `main`.
set -e
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "early"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
// A global constructor that allocates, as a test framework's registration does.
std::vector<std::string>* registered = new std::vector<std::string>{std::string(64, 'x')};
int main() {
    std::println("registered {}", registered->front().size());
    return 0;
}
CPP

"$MCPP" build --target aarch64-macos-musl > build.log 2>&1 || fail "the hosted macOS build" build.log
grep -q "openkal-platform" build.log || {
    echo "SKIP: aarch64-macos-musl is not served by openkal-platform's hosted arrangement here"
    exit 77
}
bin=$(find target -path '*/bin/early' -type f | head -1)
[ -n "$bin" ] || fail "no image was produced" build.log

# A
objdump="$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-objdump"
nm="$_e2e_registry_base/xim-x-llvm/${LLVM_VERSION}/bin/llvm-nm"
[ -x "$objdump" ] && [ -x "$nm" ] || { echo "SKIP(A): no llvm@${LLVM_VERSION} payload to read the image with"; objdump=""; }
if [ -n "$objdump" ]; then
    python3 - "$bin" "$objdump" "$nm" > a.log 2>&1 <<'PY' || fail "A: the order of the image's initialisers" a.log build.log
import subprocess, sys
image, objdump, nm = sys.argv[1:4]
dump = subprocess.run([objdump, "--macho", "-s", "-j", "__init_offsets", image],
                      capture_output=True, text=True, check=True).stdout
words = []
for line in dump.splitlines():
    parts = line.split()
    if len(parts) < 2 or not all(c in "0123456789abcdef" for c in parts[0]):
        continue
    for w in parts[1:5]:
        if len(w) == 8 and all(c in "0123456789abcdef" for c in w):
            words.append(int.from_bytes(bytes.fromhex(w), "little"))
symbols = []
for line in subprocess.run([nm, "-n", image], capture_output=True, text=True, check=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3:
        symbols.append((int(parts[0], 16), parts[2]))
def name_at(offset):
    address = 0x100000000 + offset
    best = None
    for value, name in symbols:
        if value > address:
            break
        best = name
    return best or "?"
names = [name_at(o) for o in words]
print("initialisers:", names[:8])
assert any("okm_early_init" in n for n in names), "the C library's initialiser is not in the image"
early = next(i for i, n in enumerate(names) if "okm_early_init" in n)
program = [i for i, n in enumerate(names) if "main.cpp" in n]
assert program, "the program's initialiser is not in the image"
assert early < program[0], f"the program's initialiser runs at {program[0]}, before the C library's at {early}"
PY
    echo "  ok  the C library's initialiser precedes the program's"
fi

# B
if [ "$(uname -s)-$(uname -m)" = Darwin-arm64 ]; then
    "$bin" > b.log 2>&1 || fail "B: the image ended with $?" b.log
    grep -q "registered 64" b.log || fail "B: the program's output" b.log
    echo "  ok  the program runs, its constructor allocating before main"
fi

echo "OK: a Mach-O image's C library is up before its initialisers"
