#!/usr/bin/env bash
# requires: llvm python3 unix-shell
# 955 -- a universal package (SPEC-014 §3, §4.2): the platform of the ok-image
# targets states `universal = true`, so `mcpp pack` of either target packs both
# into one package, named after the platform, with a launcher that picks this
# machine's architecture and starts the platform's runner.
#
# Criteria:
#   A. `<name>-<version>-openkal/` holds `lib/x86_64/` and `lib/aarch64/` (each
#      the image of its architecture), `bin/<name>` and `bin/<name>.cmd`, and
#      `mcpp-package.json` naming both legs, the runner and each entry.
#   B. `bin/<name>` runs the image of this machine's architecture through the
#      runner found on PATH, with its arguments.
#   C. `--mode self-contained` adds the runner's program for this system and
#      each architecture under `libexec/<runner>/<os>-<arch>/`, and the launcher
#      runs with no runner on PATH.
#   D. `--no-universal` packs the selected target alone.
#   E. (Linux) `--systems linux,macos,windows` carries the runner of each
#      system this mcpp builds one for, made here over the platform's hosted
#      arrangement: a Mach-O for macos-aarch64, a PE for windows-x86_64, and
#      none of them with the runner's own package description beside it.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64;  OTHER=aarch64 ;;
    aarch64|arm64) HOST_ARCH=aarch64; OTHER=x86_64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
case "$(uname -s)" in Linux) OS=linux ;; Darwin) OS=macos ;; *) echo "SKIP: a POSIX launcher"; exit 0 ;; esac
T="$HOST_ARCH-openkal"

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "uni"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
int main(int argc, char** argv) {
    try { throw std::runtime_error("universal"); }
    catch (const std::exception& e) { std::println("{} {} args", e.what(), argc); }
    return 0;
}
CPP

# A
"$MCPP" pack --target "$T" --pkg-format dir > a.log 2>&1 || fail "A: pack" a.log
P=target/dist/uni-0.1.0-openkal
for f in "lib/x86_64/uni.oke" "lib/aarch64/uni.oke" "bin/uni" "bin/uni.cmd" "mcpp-package.json"; do
    [ -f "$P/$f" ] || fail "A: the package has no $f" a.log
done
python3 - "$P" <<'PY' || fail "A: the package's metadata" "$P/mcpp-package.json"
import json, sys
root = sys.argv[1]
d = json.load(open(root + "/mcpp-package.json"))
assert sorted(d["legs"]) == ["aarch64-openkal", "x86_64-openkal"], d["legs"]
assert d["launch"]["runner"][0].startswith("okrun"), d["launch"]
assert d["launch"]["entry"] == {"x86_64": "lib/x86_64/uni.oke", "aarch64": "lib/aarch64/uni.oke"}, d["launch"]
# Each image is of its architecture (e_machine 62 x86-64, 183 AArch64).
import struct
for arch, machine in (("x86_64", 62), ("aarch64", 183)):
    head = open(f"{root}/lib/{arch}/uni.oke", "rb").read(20)
    assert head[:4] == b"\x7fELF" and struct.unpack_from("<H", head, 18)[0] == machine, arch
roles = {f["path"]: f["role"] for f in d["files"]}
assert roles["bin/uni"] == "launcher" and roles["lib/x86_64/uni.oke"] == "image", roles
PY
echo "ok: one package holds both architectures"

# B
okrun=$("$MCPP" self env --target "$T" --format json 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["runner"]["program"])')
[ -x "$okrun" ] || fail "B: the platform's runner was not located ($okrun)"
PATH="$(dirname "$okrun"):$PATH" "$P/bin/uni" one two > b.log 2>&1 || fail "B: the launcher" b.log
grep -q "universal 3 args" b.log || fail "B: the image's output through the launcher" b.log
echo "ok: the launcher runs this machine's image through the runner"

# C
"$MCPP" pack --target "$T" --mode self-contained --pkg-format dir -o sc > c.log 2>&1 || fail "C: pack --mode self-contained" c.log
# This machine's architecture's runner is always there; the other one is there
# where this mcpp builds programs for it (not x86_64-macos, a planned row).
[ -f "target/dist/sc/libexec/okrun/$OS-$HOST_ARCH/okrun" ] || fail "C: no runner for $OS-$HOST_ARCH" c.log
if [ ! -f "target/dist/sc/libexec/okrun/$OS-$OTHER/okrun" ]; then
    grep -q "no runner for $OS-$OTHER" c.log || fail "C: the runner for $OS-$OTHER is missing and not said" c.log
fi
env -i HOME="$HOME" PATH=/usr/bin:/bin target/dist/sc/bin/uni x > c-run.log 2>&1 || fail "C: the self-contained launcher" c-run.log
grep -q "universal 2 args" c-run.log || fail "C: the self-contained package's output" c-run.log
python3 - <<PY || fail "C: systems" target/dist/sc/mcpp-package.json
import json, os
d = json.load(open("target/dist/sc/mcpp-package.json"))
carried = sorted(s for s in os.listdir("target/dist/sc/libexec/okrun"))
assert d["mode"] == "self-contained" and d["systems"] == carried and "$OS-$HOST_ARCH" in carried, d
PY
echo "ok: a self-contained universal package carries its runner and needs nothing installed"

# D
"$MCPP" pack --target "$T" --no-universal --pkg-format dir -o one > d.log 2>&1 || fail "D: --no-universal" d.log
[ -f target/dist/one/bin/uni.oke ] || fail "D: --no-universal did not pack the target alone" d.log
[ -d target/dist/one/lib/$OTHER ] && fail "D: --no-universal packed the other architecture" d.log
echo "ok: --no-universal packs the selected target alone"

# E
if [ "$OS" = linux ]; then
    "$MCPP" pack --target "$T" --mode self-contained --systems linux,macos,windows --pkg-format dir -o uni-0.1.0-all \
        > e.log 2>&1 || fail "E: --systems linux,macos,windows" e.log
    python3 - target/dist/uni-0.1.0-all <<'PY' || fail "E: the runners" e.log
import os, sys
root = os.path.join(sys.argv[1], "libexec", "okrun")
def head(p): return open(p, "rb").read(4)
assert head(os.path.join(root, "macos-aarch64", "okrun")) == b"\xcf\xfa\xed\xfe", "macos-aarch64 is not a Mach-O"
assert head(os.path.join(root, "windows-x86_64", "okrun.exe"))[:2] == b"MZ", "windows-x86_64 is not a PE"
for d, _, files in os.walk(root):
    assert "mcpp-package.json" not in files, d
PY
    grep -q "no runner for macos-x86_64" e.log || fail "E: the planned row is not said" e.log
    echo "ok: the runners of other systems are made here, over the hosted arrangement"
fi

echo "PASS: 955 a universal package holds every architecture"
