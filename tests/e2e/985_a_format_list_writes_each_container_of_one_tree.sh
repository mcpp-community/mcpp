#!/usr/bin/env bash
# requires: llvm python3 unix-shell
# 985 -- a `--pkg-format` list writes each container of one staged tree
# (design 2026-10-29 rev 2 §4.6).
#
# A list names the containers of one tree: the engine's `tar` and `zip` and a
# provider's format (`okb` of openkal-platform). The pack builds once, stages
# once and writes each container from that tree; a list takes a directory for
# `-o`, and refuses what is no container of a tree. On 2026.10.29.1
# `--pkg-format tar,zip` was an unknown format.
#
# Criteria:
#   A. `--pkg-format tar,zip -o out/` writes both containers of one tree below
#      `out/`, holding the same files; the envelope's `pkgFormat` is `tar` and
#      `pkgFormats` is `["tar", "zip"]`.
#   B. Refused before a build: `dir` or `single-file` in a list; `-o` naming
#      one file with a list; `mcpp run --pkg-format tar,zip`.
#   C. `--target openkal --mode self-contained --pkg-format tar,okb` writes the
#      tree's tar and the platform's okb; `--pkg-format okb` alone is refused
#      under `self-contained`, saying the runner would be dropped.
#   D. `-o target/dist/`, the directory the containers are written in anyway,
#      keeps them: the okb is not removed as the destination of its own copy.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "lst"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("list"); }\n' > src/main.cpp

# A
"$MCPP" pack --pkg-format tar,zip -o out/ --message-format json > a.json 2> a.log || fail "A: tar,zip" a.log a.json
python3 - a.json <<'PY' || fail "A: the containers" a.json a.log
import json, os, sys, tarfile, zipfile
d = json.load(open(sys.argv[1]))["data"]
assert d["pkgFormat"] == "tar" and d["pkgFormats"] == ["tar", "zip"], d
paths = [a["path"] for a in d["artifacts"]]
assert len(paths) == 2 and all(os.path.dirname(p).endswith("/out") for p in paths), paths
tgz = next(p for p in paths if p.endswith(".tar.gz"))
zp = next(p for p in paths if p.endswith(".zip"))
with tarfile.open(tgz) as t:
    in_tar = sorted(m.name for m in t.getmembers() if m.isfile())
with zipfile.ZipFile(zp) as z:
    in_zip = sorted(n for n in z.namelist() if not n.endswith("/"))
assert in_tar == in_zip, (in_tar, in_zip)
PY
[ "$(grep -c "Compiling lst" a.log)" -le 1 ] || fail "A: the program was compiled more than once" a.log
echo "ok: a list writes each container of one tree"

# B
refused() {
    local label=$1; shift
    if "$@" > b.log 2>&1; then fail "B: $label was accepted" b.log; fi
    grep -q "Compiling" b.log && fail "B: $label was refused after compiling" b.log
    return 0
}
refused "dir in a list" "$MCPP" pack --pkg-format tar,dir
grep -q "lists the containers of one staged tree" b.log || fail "B: the refusal of dir" b.log
refused "single-file in a list" "$MCPP" pack --pkg-format single-file,tar
refused "-o one file with a list" "$MCPP" pack --pkg-format tar,zip -o one.tar.gz
grep -q "use: -o <directory>/" b.log || fail "B: the refusal of -o" b.log
refused "mcpp run with a list" "$MCPP" run --pkg-format tar,zip
grep -q "names one format" b.log || fail "B: the refusal of run" b.log
echo "ok: what is no container of a tree, and -o naming one file, are refused"

# C
case "$(uname -m)" in x86_64|amd64|aarch64|arm64) ;; *) echo "PASS: 985 (no ok-image row here)"; exit 0 ;; esac
"$MCPP" pack --target openkal --mode self-contained --pkg-format tar,okb -o kit/ > c.log 2>&1 || fail "C: tar,okb" c.log
ls kit/lst-0.1.0-openkal-bundle-all.tar.gz > /dev/null 2>&1 || fail "C: no tar" c.log
ls kit/*.okb > /dev/null 2>&1 || fail "C: no okb" c.log
if "$MCPP" pack --target openkal --mode self-contained --pkg-format okb > c2.log 2>&1; then
    fail "C: okb alone was accepted under self-contained" c2.log
fi
grep -q "the runner would be dropped" c2.log || fail "C: the refusal" c2.log
grep -q "use: --pkg-format tar,okb" c2.log || fail "C: the refusal names no remedy" c2.log
echo "ok: tar,okb writes both; okb alone does not drop a self-contained package's runner"

# D
"$MCPP" pack --target openkal --mode self-contained --systems all --pkg-format tar,okb -o target/dist/ \
    > d.log 2>&1 || fail "D: -o target/dist/" d.log
ls target/dist/lst-0.1.0-openkal-bundle-all.tar.gz > /dev/null 2>&1 || fail "D: no tar in target/dist/" d.log
ls target/dist/lst-0.1.0.okb > /dev/null 2>&1 || fail "D: no okb in target/dist/" d.log
echo "ok: -o target/dist/ keeps what is already written there"

echo "PASS: 985 a format list writes each container of one tree"
