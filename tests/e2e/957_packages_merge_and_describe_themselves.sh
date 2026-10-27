#!/usr/bin/env bash
# requires: llvm python3 unix-shell
# 957 -- packages written on several machines merge into one (SPEC-014 §4.6),
# and a package describes its own distribution entry (§8).
#
# A self-contained universal package carries the runner of the systems it was
# packed for. One packed on Linux and one packed on macOS hold the same images
# and different runners; `--merge` makes the package for both. Here the second
# is the first with its runner moved to another system's directory, which is
# what a package packed there holds.
#
# Criteria:
#   A. `--merge a --merge b` writes one package whose legs and systems are the
#      union, every file of both, and a `mcpp-package.json` that describes it.
#   B. A file both packages hold with different digests is refused, naming the
#      file and both digests; packages of different versions are refused.
#   C. `mcpp emit xpkg --package` of a universal package is one asset for
#      linux, macosx and windows, with the runner as a dependency when the
#      package does not carry it.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  T=x86_64-openkal ;;
    aarch64|arm64) T=aarch64-openkal ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "mrg"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("merged"); }\n' > src/main.cpp
"$MCPP" pack --target "$T" --mode self-contained --pkg-format dir -o here > pack.log 2>&1 || fail "pack" pack.log

# The same package as another system packs it: its runner in that system's
# directory, and the metadata saying so.
python3 - target/dist/here "$TMP/there" <<'PY' || fail "could not shape the second package"
import json, os, shutil, sys
src, dst = sys.argv[1], sys.argv[2]
shutil.copytree(src, dst, symlinks=True)
d = json.load(open(os.path.join(dst, "mcpp-package.json")))
here = os.path.join(dst, "libexec", "okrun")
moved = {}
for sysdir in os.listdir(here):
    other = sysdir.replace("linux-", "other-").replace("macos-", "other-")
    os.rename(os.path.join(here, sysdir), os.path.join(here, other))
    moved[sysdir] = other
for f in d["files"]:
    for a, b in moved.items():
        f["path"] = f["path"].replace(f"libexec/okrun/{a}/", f"libexec/okrun/{b}/")
d["systems"] = sorted(moved.values())
json.dump(d, open(os.path.join(dst, "mcpp-package.json"), "w"), indent=2)
PY

# A
"$MCPP" pack --merge target/dist/here --merge "$TMP/there" -o "$TMP/merged.zip" > a.log 2>&1 || fail "A: --merge" a.log
python3 - "$TMP/merged.zip" <<'PY' || fail "A: the merged package" a.log
import hashlib, json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
names = z.namelist()
top = names[0].split("/")[0]
d = json.loads(z.read(f"{top}/mcpp-package.json"))
assert len(d["systems"]) == 4 and any(s.startswith("other-") for s in d["systems"]), d["systems"]
assert sorted(d["legs"]) == ["aarch64-openkal", "x86_64-openkal"], d["legs"]
for f in d["files"]:
    data = z.read(f"{top}/{f['path']}")
    assert hashlib.sha256(data).hexdigest() == f["sha256"], f["path"]
PY
echo "ok: --merge writes the union of the packages"

# B
python3 - "$TMP/there" <<'PY'
import hashlib, json, os, sys
root = sys.argv[1]
img = os.path.join(root, "lib", "x86_64", "mrg.oke")
open(img, "ab").write(b"\0")
d = json.load(open(os.path.join(root, "mcpp-package.json")))
for f in d["files"]:
    if f["path"] == "lib/x86_64/mrg.oke":
        f["sha256"] = hashlib.sha256(open(img, "rb").read()).hexdigest()
json.dump(d, open(os.path.join(root, "mcpp-package.json"), "w"), indent=2)
PY
if "$MCPP" pack --merge target/dist/here --merge "$TMP/there" -o "$TMP/bad.zip" > b.log 2>&1; then
    fail "B: two digests of one image were merged" b.log
fi
grep -q "lib/x86_64/mrg.oke" b.log || fail "B: the refusal does not name the file" b.log
python3 - "$TMP/there" <<'PY'
import json, os, sys
p = os.path.join(sys.argv[1], "mcpp-package.json")
d = json.load(open(p)); d["version"] = "0.2.0"; json.dump(d, open(p, "w"))
PY
if "$MCPP" pack --merge target/dist/here --merge "$TMP/there" -o "$TMP/bad2.zip" > b2.log 2>&1; then
    fail "B: two versions were merged" b2.log
fi
grep -q "version" b2.log || fail "B: the refusal does not name the version" b2.log
echo "ok: --merge refuses what is not one package"

# C
"$MCPP" pack --target "$T" --pkg-format zip -o mrg-0.1.0-openkal.zip > c.log 2>&1 || fail "C: pack" c.log
"$MCPP" emit xpkg --package target/dist/mrg-0.1.0-openkal.zip --url https://example.org/v0.1.0 > xpkg.lua 2> c.err \
    || fail "C: emit xpkg" c.err
python3 - xpkg.lua <<'PY' || fail "C: the entry" xpkg.lua
import hashlib, re, sys
text = open(sys.argv[1]).read()
sha = hashlib.sha256(open("target/dist/mrg-0.1.0-openkal.zip", "rb").read()).hexdigest()
for system in ("linux", "macosx", "windows"):
    block = re.search(system + r" = \{(.*?)\n        \},", text, re.S)
    assert block, system
    b = block.group(1)
    assert "https://example.org/v0.1.0/mrg-0.1.0-openkal.zip" in b and sha in b, (system, b)
    assert 'deps = { "okrun" }' in b, (system, b)
assert 'xvm.add("mrg"' in text
PY
echo "ok: a universal package is one asset for every system"

echo "PASS: 957 packages merge and describe themselves"
