#!/usr/bin/env bash
# requires: llvm python3 unix-shell
# 983 -- a pack holds the legs it names (SPEC-014 §1.4, §3; design 2026-10-29
# rev 2 §4.2-§4.4, §4.8).
#
# A platform no longer packs more than the command named: `--target
# x86_64-openkal` is one leg, `--target openkal` (a target set) is every
# architecture of the platform, and an image of a platform that states a
# runner is packed as one layout whatever the number of legs -- launchers in
# `bin/`, the images in `lib/<arch>/`, under `self-contained` the runner per
# system and architecture in `libexec/`. The package says where it runs. On
# 2026.10.29.1 `--target x86_64-openkal` packed both architectures, and
# `--target openkal` was an unknown target.
#
# Criteria:
#   A. `--target <host>-openkal` writes `<name>-<version>-<host>-openkal/` with
#      `lib/<host>/` alone, `bin/<name>` and `bin/<name>.exe`, its metadata
#      naming the one leg; the summary says it holds one architecture and how
#      to pack them all.
#   B. `--target openkal` writes `<name>-<version>-openkal/` with both legs.
#   C. `--mode self-contained --systems <os>-<host>`: the runner of that pair
#      alone; `Runs on` names it, and `mcpp-package.json` `runs_on.self` is it.
#   D. `--message-format json`: `legs`, `pkgFormats` and `runsOn` describe the
#      package.
#   E. `--merge` of two one-leg packages writes the tree of both, named after
#      the platform, with launchers written for both legs (the two inputs'
#      launchers differ and are not a conflict); it runs here.
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
HT="$HOST_ARCH-openkal"

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "legs"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
int main(int argc, char**) { std::println("legs {} args", argc); }
CPP

# A
"$MCPP" pack --target "$HT" --pkg-format dir > a.log 2>&1 || fail "A: pack" a.log
P="target/dist/legs-0.1.0-$HT"
for f in "lib/$HOST_ARCH/legs.oke" "bin/legs" "bin/legs.exe" "mcpp-package.json"; do
    [ -f "$P/$f" ] || fail "A: the package has no $f" a.log
done
[ -d "$P/lib/$OTHER" ] && fail "A: one leg named, and the package holds $OTHER" a.log
[ -d target/dist/legs-0.1.0-openkal ] && fail "A: one leg named, and a tree of the platform was written" a.log
python3 - "$P/mcpp-package.json" "$HT" <<'PY' || fail "A: the metadata" "$P/mcpp-package.json"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["legs"] == [sys.argv[2]], d["legs"]
assert d["launch"]["runner"][0].startswith("okrun"), d["launch"]
PY
grep -q "only $HOST_ARCH; \`--target openkal\` or \`\[pack\] targets = \[\"openkal\"\]\` packs every architecture" a.log \
    || fail "A: the summary does not say how to pack every architecture" a.log
echo "ok: one leg named is one leg packed, in the tree layout"

# B
"$MCPP" pack --target openkal --pkg-format dir > b.log 2>&1 || fail "B: --target openkal" b.log
B=target/dist/legs-0.1.0-openkal
for f in "lib/x86_64/legs.oke" "lib/aarch64/legs.oke" "bin/legs" "bin/legs.exe"; do
    [ -f "$B/$f" ] || fail "B: the package has no $f" b.log
done
grep -q "only $HOST_ARCH" b.log && fail "B: the summary of both legs says one" b.log
okrun=$("$MCPP" self env --target "$HT" --format json 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["runner"]["program"])')
PATH="$(dirname "$okrun"):$PATH" "$B/bin/legs" x y > b-run.log 2>&1 || fail "B: the launcher" b-run.log
grep -q "legs 3 args" b-run.log || fail "B: the launcher's output" b-run.log
echo "ok: the target set is one tree of every architecture"

# C
"$MCPP" pack --target openkal --mode self-contained --systems "$OS-$HOST_ARCH" --pkg-format dir -o one-runner \
    > c.log 2>&1 || fail "C: --systems $OS-$HOST_ARCH" c.log
C=target/dist/one-runner
[ -d "$C/libexec/okrun/$OS-$HOST_ARCH" ] || fail "C: no runner for $OS-$HOST_ARCH" c.log
[ "$(ls "$C/libexec/okrun")" = "$OS-$HOST_ARCH" ] || fail "C: runners other than $OS-$HOST_ARCH" c.log
grep -q "Runs on $OS-$HOST_ARCH" c.log || fail "C: the summary does not say where it runs" c.log
python3 - "$C/mcpp-package.json" "$OS-$HOST_ARCH" <<'PY' || fail "C: runs_on" "$C/mcpp-package.json"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["runs_on"]["self"] == [sys.argv[2]], d["runs_on"]
assert d["systems"] == [sys.argv[2]], d["systems"]
PY
env -i HOME="$HOME" PATH=/usr/bin:/bin "$C/bin/legs" z > c-run.log 2>&1 || fail "C: the self-contained launcher" c-run.log
grep -q "legs 2 args" c-run.log || fail "C: the self-contained package's output" c-run.log
echo "ok: --systems <os>-<arch> carries that runner alone, and the package says where it runs"

# D
"$MCPP" pack --target openkal --mode self-contained --systems "$OS" --message-format json > d.json 2> d.err \
    || fail "D: json" d.err d.json
python3 - d.json "$OS-$HOST_ARCH" <<'PY' || fail "D: the envelope" d.json
import json, sys
d = json.load(open(sys.argv[1]))["data"]
assert d["legs"] == ["x86_64-openkal", "aarch64-openkal"], d["legs"]
assert d["pkgFormat"] == "tar" and d["pkgFormats"] == ["tar"], d
assert sys.argv[2] in d["runsOn"]["self"] and isinstance(d["runsOn"]["with"], dict), d["runsOn"]
PY
echo "ok: the envelope names the legs, the formats and where the package runs"

# E
"$MCPP" pack --target "$HT" --mode self-contained --pkg-format dir > e1.log 2>&1 || fail "E: pack $HT" e1.log
"$MCPP" pack --target "$OTHER-openkal" --mode self-contained --pkg-format dir > e2.log 2>&1 || fail "E: pack $OTHER-openkal" e2.log
"$MCPP" pack --merge "target/dist/legs-0.1.0-$HT-bundle-all" --merge "target/dist/legs-0.1.0-$OTHER-openkal-bundle-all" \
    --pkg-format dir > e.log 2>&1 || fail "E: --merge of two one-leg packages" e.log
M=legs-0.1.0-openkal-bundle-all
[ -d "$M" ] || fail "E: the merged tree is not named after the platform" e.log
grep -q "'x86_64') entry='legs.oke'" "$M/bin/legs" && grep -q "'aarch64') entry='legs.oke'" "$M/bin/legs" \
    || fail "E: the merged launcher does not name both legs" "$M/bin/legs"
env -i HOME="$HOME" PATH=/usr/bin:/bin "$M/bin/legs" q > e-run.log 2>&1 || fail "E: the merged launcher" e-run.log
grep -q "legs 2 args" e-run.log || fail "E: the merged package's output" e-run.log
echo "ok: --merge writes the launchers of the merged legs"

echo "PASS: 983 a pack holds the legs it names"
