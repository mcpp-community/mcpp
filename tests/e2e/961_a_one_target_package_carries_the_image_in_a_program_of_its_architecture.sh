#!/usr/bin/env bash
# requires: llvm python3
# 961 -- a self-contained package of one ok-image target is a program of the
# image's architecture, and `--list-formats` lists what the target offers
# (SPEC-014 §1.2, §6; SPEC-013 §11; design 2026-10-29).
#
# Measured on 2026.10.28.1, on x86_64: `mcpp pack --target aarch64-openkal
# --no-universal --mode self-contained` wrote an x86-64 program carrying an
# aarch64 image, which runs on no machine; `mcpp pack --target aarch64-openkal
# --list-formats` planned this machine and left out the platform's `okb`.
#
# Criteria:
#   A. The package's `bin/app` is an ELF of the image's architecture; where
#      this machine has that architecture's qemu, it runs the image.
#   B. `mcpp pack --target <t>-openkal --list-formats` lists `okb`, provided by
#      the platform; `mcpp pack --list-formats` for this machine does not.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  OTHER=aarch64 ;;
    aarch64|arm64) OTHER=x86_64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
OT="$OTHER-openkal"

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
int main(int argc, char** argv) {
    try { throw std::runtime_error("caught"); }
    catch (const std::exception& e) { std::println("image: {} ({} args)", e.what(), argc); }
    return 0;
}
CPP

# A
"$MCPP" pack --target "$OT" --no-universal --mode self-contained --pkg-format dir -o m3 > a.log 2>&1 \
    || fail "A: pack" a.log
[ -f target/dist/m3/bin/app ] || fail "A: the package has no program" a.log
python3 - target/dist/m3/bin/app "$OTHER" <<'PY' || fail "A: the program's architecture" a.log
import struct, sys
d = open(sys.argv[1], "rb").read(20)
assert d[:4] == b"\x7fELF", "not an ELF file"
m = {62: "x86_64", 183: "aarch64"}.get(struct.unpack("<H", d[18:20])[0])
assert m == sys.argv[2], f"the carrier is a program of {m}, the image is {sys.argv[2]}"
PY
grep -q "into a program of linux-$OTHER" a.log || fail "A: the Packing line names another architecture" a.log
QEMU=$(command -v "qemu-$OTHER" || command -v "qemu-$OTHER-static" || true)
if [ -n "$QEMU" ]; then
    "$QEMU" target/dist/m3/bin/app x > a-run.log 2>&1 || fail "A: the program under $QEMU" a-run.log
    grep -q "image: caught (2 args)" a-run.log || fail "A: the program's image under $QEMU" a-run.log
    echo "  ok: $QEMU runs it"
fi
echo "ok: a one-target self-contained package is a program of the image's architecture"

# B
"$MCPP" pack --target "$OT" --list-formats > b.json 2> b.err || fail "B: --list-formats --target $OT" b.err b.json
"$MCPP" pack --list-formats > b-host.json 2> b-host.err || fail "B: --list-formats" b-host.err b-host.json
python3 - b.json b-host.json <<'PY' || fail "B: the formats" b.json b-host.json
import json, sys
target = {f["name"]: f for f in json.load(open(sys.argv[1]))["data"]["formats"]}
host = {f["name"] for f in json.load(open(sys.argv[2]))["data"]["formats"]}
assert "okb" in target, sorted(target)
assert any(p.endswith("openkal-platform") for p in target["okb"]["providers"]), target["okb"]
assert "okb" not in host, sorted(host)
PY
echo "ok: --list-formats lists the target's formats"

echo "PASS: 961 a one-target package carries the image in a program of its architecture"
