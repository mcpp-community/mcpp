#!/usr/bin/env bash
# requires: pack python3
# 723 -- `mcpp pack --message-format json` reports what it produced, and `pack`
# takes the profile shorthands `build` and `run` take, with one precedence
# (#649 E9).
#
# A driver that publishes a pack's artifacts used to parse the human `Packed`
# lines, whose paths are shortened (`@mcpp/`, `~/`, project-relative), and a
# driver mapping one profile switch onto three verbs found `--release` refused
# on the third. `mcpp run --profile dev --release` built `release` while
# `mcpp build` given the same line built `dev`.
#
# Legs:
#   A. `pack --format tar --message-format json`: stdout is one `mcpp.pack`
#      envelope and nothing else; the artifact path is absolute and exists, its
#      type is `file`, its format `tar`, its target the host triple; the staged
#      tree and its manifest exist, and the closure is `walked`.
#   B. `pack --format dir --message-format json`: the artifact is a directory.
#   C. `pack --release` and `pack --dev` are accepted; `--dev` builds `dev`.
#   D. `run --profile dev --release` builds `dev`, as `build` does.
#   E. `pack --message-format yaml` is refused with exit 2.
#   F. `--protocol-version` lists the `mcpp.pack` kind and the `pack` command.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.3.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
printf '#include <cstdio>\nint main() { std::puts("app ran"); return 0; }\n' > app/src/main.cpp
cd app

# ── A ───────────────────────────────────────────────────────────────────────
"$MCPP" pack --format tar --message-format json > a.json 2> a.err || fail "A: pack failed" a.err a.json
python3 - a.json <<'EOF' || fail "A: the envelope" a.json a.err
import json, os, sys
text = open(sys.argv[1]).read()
e = json.loads(text)                      # the whole of stdout is one document
assert e["kind"] == "mcpp.pack" and e["kindVersion"] == 1, (e["kind"], e.get("kindVersion"))
assert "write-project" in e["effects"], e["effects"]
arts = e["data"]["artifacts"]
assert len(arts) == 1, arts
a = arts[0]
assert os.path.isabs(a["path"]) and os.path.isfile(a["path"]), a
assert a["path"].endswith(".tar.gz") and a["type"] == "file" and a["format"] == "tar", a
assert len(a["targets"]) == 1 and a["targets"][0].endswith("linux-gnu"), a["targets"]
st = e["data"]["stage"]
assert os.path.isdir(st["dir"]) and os.path.isfile(st["manifest"]), st
assert st["closure"] == "walked", st
assert e["diagnostics"] == [], e["diagnostics"]
EOF
grep -q "Packed" a.err || fail "A: the human lines did not go to stderr" a.err
echo "ok: A, one envelope on stdout names the archive, its legs and the staged tree"

# ── B ───────────────────────────────────────────────────────────────────────
"$MCPP" pack --format dir --message-format json > b.json 2> b.err || fail "B: pack failed" b.err
python3 - b.json <<'EOF' || fail "B: the envelope" b.json
import json, os, sys
a = json.load(open(sys.argv[1]))["data"]["artifacts"][0]
assert a["type"] == "directory" and os.path.isdir(a["path"]) and a["format"] == "dir", a
EOF
echo "ok: B, a directory artifact is reported as one"

# ── C ───────────────────────────────────────────────────────────────────────
# `pack` narrates no `Finished` line, so the profile is read from the graph the
# pack built: `dev` compiles at `-O0`, `release` does not. Each sub-leg starts
# from an empty `target/`, so the one graph there is the one this pack wrote.
built_dev() { grep -q -- " -O0" "$(ls target/*-linux-gnu/*/build.ninja | head -1)"; }
rm -rf target
"$MCPP" pack --release --format tar > c1.log 2>&1 || fail "C: pack --release was refused" c1.log
built_dev && fail "C: pack --release built dev" c1.log
rm -rf target
"$MCPP" pack --dev --format tar > c2.log 2>&1 || fail "C: pack --dev was refused" c2.log
built_dev || fail "C: pack --dev did not build dev" c2.log
rm -rf target
"$MCPP" pack --profile release --dev --format tar > c3.log 2>&1 || fail "C: pack --profile --dev failed" c3.log
built_dev && fail "C: --dev won over --profile release on pack" c3.log
echo "ok: C, pack takes --release and --dev, and --profile wins"

# ── D ───────────────────────────────────────────────────────────────────────
"$MCPP" run --profile dev --release > d.log 2>&1 || fail "D: run failed" d.log
grep -q "Finished dev" d.log || fail "D: run --profile dev --release did not build dev" d.log
"$MCPP" build --profile dev --release > d2.log 2>&1 || fail "D: build failed" d2.log
grep -q "Finished dev" d2.log || fail "D: build --profile dev --release did not build dev" d2.log
echo "ok: D, run and build resolve --profile over the shorthands alike"

# ── E ───────────────────────────────────────────────────────────────────────
set +e
"$MCPP" pack --format tar --message-format yaml > e.out 2> e.err
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "E: --message-format yaml exited $rc" e.err
grep -q "unknown --message-format 'yaml'" e.err || fail "E: the refusal does not name the value" e.err
echo "ok: E, an unknown message format is refused"

# ── F ───────────────────────────────────────────────────────────────────────
"$MCPP" --protocol-version > f.json
python3 - f.json <<'EOF' || fail "F: the protocol document" f.json
import json, sys
p = json.load(open(sys.argv[1]))
assert p["kinds"]["mcpp.pack"] == 1, p["kinds"]
fx = p["commands"]["pack"]["effects"]
assert "write-project" in fx and "exec-build-script" in fx, fx
EOF
echo "ok: F, the protocol document declares the kind and the command"

echo "PASS: 723"
