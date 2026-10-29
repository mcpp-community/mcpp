#!/usr/bin/env bash
# requires: llvm python3
# 987 -- the spellings of the universal package are migrated (design
# 2026-10-29 rev 2 §13.2, §4.8; docs/51).
#
# A pack's targets are its legs, so nothing turns a platform's targets into
# legs any more. `--no-universal` with one target is what the pack does and
# is said; with a target set it contradicts the set. `[pack] universal =
# true` asked for every target and is refused with the spelling that does
# it; `false` is said and ignored. Each is in `mcpp why compat`. On
# 2026.10.29.1 `[pack] universal = true` was accepted.
#
# Criteria:
#   A. `--no-universal --target <host>-openkal` packs the one leg and notes the
#      layout (`--pkg-format single-file` for one program).
#   B. `--no-universal --target openkal` is refused: the two contradict.
#   C. `[pack] universal = true` is refused naming `[pack] targets =
#      ["openkal"]`; `false` is noted and the pack proceeds.
#   D. `[pack] targets = ["openkal"]` packs both legs with no `--target`.
#   E. `mcpp pack --help` puts the common options first, then the rest.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64 ;;
    aarch64|arm64) HOST_ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
HT="$HOST_ARCH-openkal"

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "mig"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("mig"); }\n' > src/main.cpp

# A
"$MCPP" pack --no-universal --target "$HT" --pkg-format dir > a.log 2>&1 || fail "A: --no-universal" a.log
[ -f "target/dist/mig-0.1.0-$HT/lib/$HOST_ARCH/mig.oke" ] || fail "A: the one leg's tree" a.log
grep -q "note: --no-universal is what a pack of one target does" a.log || fail "A: the note" a.log
grep -q -- "--pkg-format single-file" a.log || fail "A: the note names no single file" a.log
echo "ok: --no-universal with one target is noted"

# B
if "$MCPP" pack --no-universal --target openkal > b.log 2>&1; then fail "B: --no-universal with a set was accepted" b.log; fi
grep -q "contradict" b.log || fail "B: the refusal" b.log
echo "ok: --no-universal with a set is refused"

# C
printf '\n[pack]\nuniversal = true\n' >> mcpp.toml
if "$MCPP" pack --target "$HT" > c.log 2>&1; then fail "C: [pack] universal = true was accepted" c.log; fi
grep -q 'use: \[pack\] targets = \["openkal"\]' c.log || fail "C: the refusal names no remedy" c.log
sed -i.bak 's/^universal = true$/universal = false/' mcpp.toml
"$MCPP" pack --target "$HT" --pkg-format dir > c2.log 2>&1 || fail "C: [pack] universal = false" c2.log
grep -q "note: \[pack\] universal = false is what a pack of one target does; it is ignored" c2.log \
    || fail "C: the note" c2.log
echo "ok: [pack] universal = true is refused, false is noted"

# D
sed -i.bak 's/^universal = false$/targets = ["openkal"]/' mcpp.toml
"$MCPP" pack --pkg-format dir > d.log 2>&1 || fail "D: [pack] targets" d.log
for arch in x86_64 aarch64; do
    [ -f "target/dist/mig-0.1.0-openkal/lib/$arch/mig.oke" ] || fail "D: no $arch leg" d.log
done
echo "ok: [pack] targets names the legs of a pack"

# E
"$MCPP" pack --help > e.log 2>&1 || fail "E: pack --help" e.log
python3 - e.log <<'PY' || fail "E: the help's groups" e.log
import sys
text = open(sys.argv[1]).read()
common, advanced = text.index("Options:"), text.index("Advanced options:")
assert common < advanced, "the groups are not in order"
for flag in ("--target", "--mode", "--pkg-format", "--systems", "--linkage", "--output"):
    at = text.index(flag)
    assert common < at < advanced, flag
for flag in ("--compress", "--merge", "--no-universal"):
    assert text.index(flag) > advanced, flag
PY
echo "ok: pack --help puts the common options first"

echo "PASS: 987 the universal spellings are migrated"
