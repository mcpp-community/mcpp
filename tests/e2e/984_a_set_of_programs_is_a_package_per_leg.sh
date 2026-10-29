#!/usr/bin/env bash
# requires: gcc python3
# 984 -- a target set of programs of a system is a package per leg (design
# 2026-10-29 rev 2 §4.2).
#
# Only a platform with a runner merges legs into one tree, and only Android's
# rows are one APK; any other program is packed per leg, each package named by
# its triple. A pack that writes more than one file takes a directory for
# `-o`. On 2026.10.29.1 `--target linux-musl` was an unknown target.
#
# Criteria:
#   A. `mcpp pack --target linux-musl -o out/` writes
#      `<name>-<version>-x86_64-linux-musl-static.tar.gz` and the aarch64 one
#      below `out/`, each holding the program of its architecture.
#   B. `-o` naming one file is refused before anything is built, saying a
#      directory is wanted.
#   C. `--message-format json` lists both packages and both legs.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

[ "$(uname -s)" = Linux ] || { echo "SKIP: the linux-musl rows are built on Linux"; exit 0; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "per"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("per leg"); }\n' > src/main.cpp

# A
if ! "$MCPP" pack --target linux-musl -o out/ > a.log 2>&1; then
    if grep -qE "cannot be built on this host|install_packages failed|not found in the synced index" a.log; then
        echo "SKIP: a linux-musl row cannot be built here"; exit 0
    fi
    fail "A: pack --target linux-musl" a.log
fi
for arch in x86_64 aarch64; do
    f="out/per-0.1.0-$arch-linux-musl-static.tar.gz"
    [ -f "$f" ] || fail "A: no $f" a.log
    python3 - "$f" "$arch" <<'PY' || fail "A: the $arch package's program" a.log
import struct, sys, tarfile
want = {"x86_64": 62, "aarch64": 183}[sys.argv[2]]
with tarfile.open(sys.argv[1]) as t:
    m = next(m for m in t.getmembers() if m.name.endswith("/bin/per"))
    head = t.extractfile(m).read(20)
assert head[:4] == b"\x7fELF" and struct.unpack_from("<H", head, 18)[0] == want, head
PY
done
echo "ok: a package per leg, each named by its triple"

# B
if "$MCPP" pack --target linux-musl -o one.tar.gz > b.log 2>&1; then fail "B: -o one file was accepted" b.log; fi
grep -q "use: -o <directory>/" b.log || fail "B: the refusal does not name a directory" b.log
grep -q "Compiling" b.log && fail "B: refused after compiling" b.log
echo "ok: -o naming one file is refused for two packages"

# C
"$MCPP" pack --target linux-musl --message-format json > c.json 2> c.err || fail "C: json" c.err c.json
python3 - c.json <<'PY' || fail "C: the envelope" c.json
import json, sys
d = json.load(open(sys.argv[1]))["data"]
assert sorted(d["legs"]) == ["aarch64-linux-musl", "x86_64-linux-musl"], d["legs"]
assert len(d["artifacts"]) == 2, d["artifacts"]
PY
echo "ok: the envelope lists both packages"

echo "PASS: 984 a set of programs is a package per leg"
