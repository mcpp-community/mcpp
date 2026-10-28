#!/usr/bin/env bash
# requires: unix-shell python3
# 954 -- `mcpp pack` has three axes (SPEC-014 §1): the linkage of the program,
# the closure the package carries (`--mode`), the container it is delivered in
# (`--pkg-format`, compressed as `--compress` says); every package describes
# itself in `mcpp-package.json`; impossible combinations are refused before
# anything is built.
#
# Criteria:
#   A. `--compress zstd | xz | none` writes `.tar.zst`, `.tar.xz`, `.tar`, each
#      holding one top-level directory; `--pkg-format zip` a zip.
#   B. The tree's `mcpp-package.json` names the package, its legs and its three
#      axes, and lists every file with its role and a digest that matches it.
#   C. `--message-format json` states linkage, mode, pkgFormat and legs, and the
#      sha256 of the archive.
#   D. `[pack] format` and `[pack] compress` are the project's defaults.
#   E. Refused before a build, naming both choices: single-file with --mode
#      system; --compress on a directory; zstd in a zip; --systems for a package
#      that carries no loader.
#   F. `--format` is the earlier spelling of `--pkg-format`, with a warning.
#   G. `[pack] include` stages the files it names at their paths, less what
#      `[pack] exclude` names, and the metadata lists them as resources.
#   H. `[pack] sign` runs on each program before the container, and the
#      metadata's digests are those of the signed files.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "axes"\nversion = "0.1.0"\n' > mcpp.toml
printf 'int main() { return 0; }\n' > src/main.cpp

# A
for c in zstd xz none; do
    "$MCPP" pack --compress "$c" > "pack-$c.log" 2>&1 || fail "A: --compress $c" "pack-$c.log"
done
"$MCPP" pack --pkg-format zip > pack-zip.log 2>&1 || fail "A: --pkg-format zip" pack-zip.log
python3 - <<'PY' || fail "A: the archives"
import glob, io, lzma, tarfile, zipfile
from pathlib import Path
d = Path("target/dist")
def one_top(names):
    tops = {n.split("/")[0] for n in names}
    assert len(tops) == 1, tops
xz = glob.glob(str(d / "*.tar.xz")); assert xz, list(d.iterdir())
with tarfile.open(xz[0], "r:xz") as t: one_top(t.getnames())
plain = glob.glob(str(d / "*.tar")); assert plain
with tarfile.open(plain[0], "r:") as t: one_top(t.getnames())
zst = glob.glob(str(d / "*.tar.zst")); assert zst
assert open(zst[0], "rb").read(4) == b"\x28\xb5\x2f\xfd", "not a zstd frame"
z = glob.glob(str(d / "*.zip")); assert z
with zipfile.ZipFile(z[0]) as f:
    one_top(f.namelist())
    assert any(i.compress_type == zipfile.ZIP_DEFLATED for i in f.infolist()), "nothing deflated"
PY
echo "ok: tar compressions and zip"

# B
python3 - <<'PY' || fail "B: mcpp-package.json"
import glob, hashlib, json, os
root = sorted(p for p in glob.glob("target/dist/*") if os.path.isdir(p))[0]
d = json.load(open(os.path.join(root, "mcpp-package.json")))
assert d["schema"] == 1 and d["name"] == "axes" and d["version"] == "0.1.0", d
assert d["mode"] == "vendored" and d["platform"] is None and len(d["legs"]) == 1, d
listed = {f["path"]: f for f in d["files"]}
on_disk = set()
for dirpath, _, files in os.walk(root):
    for n in files:
        rel = os.path.relpath(os.path.join(dirpath, n), root).replace(os.sep, "/")
        if rel != "mcpp-package.json" and not os.path.islink(os.path.join(dirpath, n)):
            on_disk.add(rel)
assert on_disk == set(listed), (on_disk, set(listed))
for rel, f in listed.items():
    assert f["sha256"] == hashlib.sha256(open(os.path.join(root, rel), "rb").read()).hexdigest(), rel
assert any(f["role"] == "program" for f in d["files"]), d["files"]
PY
echo "ok: mcpp-package.json describes every file"

# C
"$MCPP" pack --compress zstd --message-format json > env.json 2> env.err || fail "C: json" env.err
python3 - env.json <<'PY' || fail "C: the envelope" env.json
import hashlib, json, sys
d = json.load(open(sys.argv[1]))["data"]
assert d["pkgFormat"] == "tar" and d["mode"] == "vendored" and d["linkage"], d
assert len(d["legs"]) == 1, d
a = d["artifacts"][0]
assert a["sha256"] == hashlib.sha256(open(a["path"], "rb").read()).hexdigest(), a
PY
echo "ok: the envelope states the axes and the digest"

# D
cat >> mcpp.toml <<'TOML'

[pack]
format   = "tar"
compress = "xz:9"
TOML
rm -rf target/dist
"$MCPP" pack > d.log 2>&1 || fail "D: pack with [pack] defaults" d.log
ls target/dist/*.tar.xz > /dev/null 2>&1 || fail "D: [pack] compress was not the default" d.log
echo "ok: [pack] format and compress are the project's defaults"

# E
refuse() {   # label, expected text, args...
    local label=$1 want=$2; shift 2
    if "$MCPP" pack "$@" > refuse.log 2>&1; then fail "E: $label was accepted" refuse.log; fi
    grep -q -- "$want" refuse.log || fail "E: $label: the refusal does not say '$want'" refuse.log
    grep -q "Compiling" refuse.log && fail "E: $label was refused after compiling" refuse.log
    return 0
}
refuse "single-file with --mode system" "single-file" --pkg-format single-file --mode system
refuse "--compress on a directory" "writes none" --pkg-format dir --compress zstd
refuse "zstd in a zip" "deflated" --pkg-format zip --compress zstd
refuse "--systems without loaders" "systems" --systems linux
echo "ok: impossible combinations are refused before a build"

# F
"$MCPP" pack --format dir > f.log 2>&1 || fail "F: --format dir" f.log
grep -q "earlier spelling of --pkg-format" f.log || fail "F: no warning for the earlier spelling" f.log
echo "ok: --format is the earlier spelling, with a warning"

# G
mkdir -p share/data
printf 'kept\n' > share/data/keep.txt
printf 'dropped\n' > share/data/drop.tmp
cat >> mcpp.toml <<'TOML'
include = ["share/**"]
exclude = ["share/**/*.tmp"]
TOML
"$MCPP" pack --pkg-format dir -o inc > g.log 2>&1 || fail "G: pack with [pack] include" g.log
[ -f target/dist/inc/share/data/keep.txt ] || fail "G: the included file is not staged" g.log
[ -f target/dist/inc/share/data/drop.tmp ] && fail "G: the excluded file was staged" g.log
python3 - <<'PY' || fail "G: the metadata" target/dist/inc/mcpp-package.json
import json
d = json.load(open("target/dist/inc/mcpp-package.json"))
roles = {f["path"]: f["role"] for f in d["files"]}
assert roles.get("share/data/keep.txt") == "resource", roles
PY
echo "ok: [pack] include stages what it names, less what exclude names"

# H
cat >> mcpp.toml <<'TOML'
sign = ["sh", "-c", "printf SIGNED >> \"$0\"", "{}"]
TOML
"$MCPP" pack --pkg-format dir -o signed > h.log 2>&1 || fail "H: pack with [pack] sign" h.log
python3 - <<'PY' || fail "H: the signed tree" target/dist/signed/mcpp-package.json
import hashlib, json, os
root = "target/dist/signed"
d = json.load(open(os.path.join(root, "mcpp-package.json")))
programs = [f for f in d["files"] if f["role"] == "program"]
assert programs, d["files"]
for f in programs:
    data = open(os.path.join(root, f["path"]), "rb").read()
    assert data.endswith(b"SIGNED"), f["path"] + " was not signed"
    assert hashlib.sha256(data).hexdigest() == f["sha256"], f["path"] + ": the digest is the unsigned file's"
for f in d["files"]:
    if f["role"] in ("resource", "metadata"):
        assert not open(os.path.join(root, f["path"]), "rb").read().endswith(b"SIGNED"), f["path"]
PY
echo "ok: [pack] sign signs each program, and the digests are the signed files'"
"$MCPP" pack --pkg-format dir -o axes-0.1.0-named > h2.log 2>&1 || fail "H: pack -o with dots" h2.log
[ -d target/dist/axes-0.1.0-named ] || fail "H: -o axes-0.1.0-named did not name the directory" h2.log
echo "ok: a directory is named as -o writes it"

echo "PASS: 954 pack has three axes"
