#!/usr/bin/env bash
# requires: llvm python3
# 961 -- a single file of an ok-image target carries the image in a program
# of the image's architecture; a self-contained package of one target is the
# tree of its platform, described for the pair its runner covers; and
# `--list-formats` lists what the target offers (SPEC-014 §1.2, §6; SPEC-013
# §11; design 2026-10-29 rev 2 §4.3, §4.4).
#
# Measured on 2026.10.28.1, on x86_64: `mcpp pack --target aarch64-openkal
# --no-universal --mode self-contained` wrote an x86-64 program carrying an
# aarch64 image, which runs on no machine; `mcpp pack --target aarch64-openkal
# --list-formats` planned this machine and left out the platform's `okb`.
# Since design 2026-10-29 the image in a program (M3) is the single file's
# alone.
#
# Criteria:
#   A. `--pkg-format single-file` of the other architecture's target is an
#      ELF of that architecture; where this machine has its qemu, it runs the
#      image.
#   B. `mcpp pack --target <t>-openkal --list-formats` lists `okb`, provided by
#      the platform; `mcpp pack --list-formats` for this machine does not.
#   C. `--mode self-contained` of that target is the tree: `lib/<arch>/`, the
#      launchers, the runner of this system and that architecture; `systems`
#      and `runs_on.self` are that pair, and `mcpp emit xpkg` describes it
#      under this system for that architecture alone. The tree without its
#      runner (`--mode vendored`) runs where the platform's runner exists:
#      described with the runner as a dependency, or refused, saying its
#      `runs_on` is empty, where the platform states no `runner.hosts`.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  OTHER=aarch64 ;;
    aarch64|arm64) OTHER=x86_64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
case "$(uname -s)" in
    Darwin) SYSTEM=macos; XSYSTEM=macosx ;;
    MINGW*|MSYS*|CYGWIN*) SYSTEM=windows; XSYSTEM=windows ;;
    *) SYSTEM=linux; XSYSTEM=linux ;;
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
"$MCPP" pack --target "$OT" --pkg-format single-file -o m3-file > a.log 2>&1 || fail "A: pack" a.log
F=target/dist/m3-file
[ -f "$F" ] || fail "A: no single file" a.log
python3 - "$F" "$OTHER" <<'PY' || fail "A: the program's architecture" a.log
import struct, sys
d = open(sys.argv[1], "rb").read(20)
assert d[:4] == b"\x7fELF", "not an ELF file"
m = {62: "x86_64", 183: "aarch64"}.get(struct.unpack("<H", d[18:20])[0])
assert m == sys.argv[2], f"the carrier is a program of {m}, the image is {sys.argv[2]}"
PY
grep -q "into a program of linux-$OTHER" a.log || fail "A: the Packing line names another architecture" a.log
QEMU=$(command -v "qemu-$OTHER" || command -v "qemu-$OTHER-static" || true)
if [ -n "$QEMU" ]; then
    "$QEMU" "$F" x > a-run.log 2>&1 || fail "A: the program under $QEMU" a-run.log
    grep -q "image: caught (2 args)" a-run.log || fail "A: the program's image under $QEMU" a-run.log
    echo "  ok: $QEMU runs it"
fi
echo "ok: a single file of one target is a program of the image's architecture"

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

# C
"$MCPP" pack --target "$OT" --mode self-contained --pkg-format dir -o m3 > c0.log 2>&1 || fail "C: pack" c0.log
for f in "lib/$OTHER/app.oke" "bin/app" "libexec/okrun/$SYSTEM-$OTHER/okrun"; do
    [ -f "target/dist/m3/$f" ] || fail "C: the tree has no $f" c0.log
done
python3 - target/dist/m3/mcpp-package.json "$SYSTEM-$OTHER" "$OTHER" <<'PY' || fail "C: the package's systems" target/dist/m3/mcpp-package.json
import json, sys
d = json.load(open(sys.argv[1]))
assert d["systems"] == [sys.argv[2]], d["systems"]
assert d["runs_on"]["self"] == [sys.argv[2]], d["runs_on"]
assert d["launch"]["entry"] == {sys.argv[3]: f"lib/{sys.argv[3]}/app.oke"}, d["launch"]
PY
tar czf m3.tar.gz -C target/dist m3
"$MCPP" emit xpkg --package m3.tar.gz --url https://example.org/v0.1.0 > c.lua 2> c.err \
    || fail "C: emit xpkg of the self-contained tree" c.err
grep -q "^        $XSYSTEM = {" c.lua || fail "C: the entry is not under $XSYSTEM" c.lua
grep -qF "archs = {\"$OTHER\"}" c.lua || fail "C: the entry's architecture is not $OTHER" c.lua
[ "$(grep -cE '^        (linux|macosx|windows) = [{]' c.lua)" = 1 ] || fail "C: the entry names more than one system" c.lua
"$MCPP" pack --target "$OT" -o bare.tar.gz > c2.log 2>&1 || fail "C: the image's vendored package" c2.log
rc=0
"$MCPP" emit xpkg --package target/dist/bare.tar.gz --url https://example.org/v0.1.0 > c2.lua 2> c2.err || rc=$?
python3 - target/dist/bare.tar.gz "$rc" c2.err c2.lua <<'PY' || fail "C: the vendored tree's entry" c2.err c2.lua
import json, sys, tarfile
with tarfile.open(sys.argv[1]) as t:
    d = json.load(t.extractfile("bare/mcpp-package.json"))
rc, err, text = int(sys.argv[2]), open(sys.argv[3]).read(), open(sys.argv[4]).read()
assert d["runs_on"]["self"] == [], d["runs_on"]
if not d["runs_on"]["with"].get("okrun"):
    assert rc != 0 and "runs_on is empty" in err, (rc, err)
else:
    assert rc == 0 and 'deps = { "xim:okrun" }' in text, (rc, err, text)
PY
echo "ok: the self-contained tree is described for its pair; the vendored tree as its runner's coverage says"

echo "PASS: 961 a single file carries the image in a program of its architecture"
