#!/usr/bin/env bash
# requires: python3
# 936 -- the control contract (design 2026-10-26 §5): what a program, an
# agent or an editor reads of a build without a terminal.
#
#   A  `mcpp build --message-format json`: NDJSON `mcpp.event` lines on stdout
#      (`plan`, `artifact`), then the `mcpp.build` envelope whose data is the
#      result record; a failing build carries the compiler's diagnostics with
#      path and line, in the events and in the record;
#   B  `mcpp run --print-launch`: the `mcpp.launch` envelope (argv, env, cwd),
#      and the program is not started;
#   C  `--deny write-project` stops before the build writes, exit 5, naming the
#      effect; an unknown effect is a usage error (exit 2);
#   D  CLICOLOR_FORCE colours a pipe, NO_COLOR wins over it;
#   E  `--home <dir>` is MCPP_HOME for one run (`self env` reports it);
#   F  `search --format json` and `index list --format json` answer with their
#      envelopes; `pack --list-formats` names tar and dir;
#   G  `--protocol-version` lists the new kinds;
#   H  a build ended by SIGTERM while it writes its directory exits 143 and
#      leaves its record `cancelled` (and `.last-result.json` says so); the
#      next build completes.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

cd "$TMP"
"$MCPP" new app > /dev/null
cd app

# A
"$MCPP" build --message-format json > a.out 2> a.err || fail "A: the build failed" a.err
$PY - a.out <<'EOF' || fail "A: the stream is not as specified" a.out
import json, sys, os
same = lambda a, b: os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))
lines = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
events = [l for l in lines if l.get("kind") == "mcpp.event"]
assert any(e["event"] == "plan" for e in events), "no plan event"
assert any(e["event"] == "artifact" and e["artifact"]["kind"] == "bin" for e in events), "no artifact event"
env = lines[-1]
assert env["kind"] == "mcpp.build" and env["schemaVersion"] == 1, env
data = env["data"]
assert data["status"] == "ok", data["status"]
art = [a for a in data["artifacts"] if a["kind"] == "bin"][0]
assert same(art["launch"]["argv"][0], art["path"]), art
EOF
printf 'int main() { return undeclared_name; }\n' > src/main.cpp
set +e
"$MCPP" build --message-format json > a2.out 2> a2.err; rc=$?
set -e
[[ $rc -ne 0 ]] || fail "A: a failing build exited 0" a2.err
$PY - a2.out <<'EOF' || fail "A: the failure carries no located diagnostic" a2.out
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
diags = [l["diagnostic"] for l in lines if l.get("event") == "diagnostic"]
assert diags, "no diagnostic event"
assert any(d["path"].endswith("main.cpp") and d["range"]["start"]["line"] == 1 for d in diags), diags
env = lines[-1]
assert env["data"]["status"] == "failed", env["data"]["status"]
assert env["data"]["diagnostics"], "the record has no diagnostics"
EOF
printf 'import std;\nint main() { std::println("started"); }\n' > src/main.cpp

# B
"$MCPP" run --print-launch > b.out 2> b.err || fail "B: --print-launch failed" b.err
grep -q started b.out && fail "B: the program was started" b.out
$PY - b.out <<'EOF' || fail "B: not an mcpp.launch envelope" b.out
import json, sys
d = json.load(open(sys.argv[1]))
assert d["kind"] == "mcpp.launch", d["kind"]
assert d["data"]["argv"] and d["data"]["argv"][0] == d["data"]["program"], d["data"]
assert "env" in d["data"] and "cwd" in d["data"]
EOF

# C
rm -rf target
set +e
"$MCPP" --deny write-project build > c.out 2> c.err; rc=$?
set -e
[[ $rc -eq 5 ]] || fail "C: --deny write-project exited $rc, expected 5" c.err
grep -q "effect denied: write-project" c.err || fail "C: the effect is not named" c.err
[[ ! -d target ]] || [[ -z "$(find target -name build.ninja)" ]] || fail "C: the project was written" c.err
set +e
"$MCPP" --deny no-such-effect build > c2.out 2> c2.err; rc=$?
set -e
[[ $rc -eq 2 ]] || fail "C: an unknown effect exited $rc, expected 2" c2.err

# D
CLICOLOR_FORCE=1 "$MCPP" build 2> d.err > /dev/null || fail "D: the build failed" d.err
grep -q $'\033\[' d.err || fail "D: CLICOLOR_FORCE did not colour a pipe" d.err
CLICOLOR_FORCE=1 NO_COLOR=1 "$MCPP" build 2> d2.err > /dev/null || fail "D: the build failed" d2.err
grep -q $'\033\[' d2.err && fail "D: NO_COLOR did not win" d2.err

# E
OTHER="$TMP/other-home"
mkdir -p "$OTHER"
"$MCPP" --home "$OTHER" self env --format json > e.out 2> e.err || true
$PY - e.out "$OTHER" <<'EOF' || fail "E: --home did not select the home for the run" e.out e.err
import json, sys, os
d = json.load(open(sys.argv[1]))
want = os.path.realpath(sys.argv[2])
blob = json.dumps(d["data"])
assert want in blob or sys.argv[2] in blob, blob[:400]
EOF

# F
"$MCPP" search --format json fmt > f1.out 2> f1.err || true
$PY -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["kind"]=="mcpp.search"; assert "packages" in d.get("data", {}) or d["diagnostics"]' f1.out \
    || fail "F: search --format json" f1.out f1.err
"$MCPP" index list --format json > f2.out 2> f2.err || fail "F: index list --format json failed" f2.err
$PY -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["kind"]=="mcpp.index"; assert isinstance(d["data"]["registries"], list)' f2.out \
    || fail "F: index list --format json" f2.out
"$MCPP" pack --list-formats > f3.out 2> f3.err || fail "F: pack --list-formats failed" f3.err
$PY -c 'import json,sys; d=json.load(open(sys.argv[1])); n=[f["name"] for f in d["data"]["formats"]]; assert d["kind"]=="mcpp.pack.formats" and "tar" in n and "dir" in n, n' f3.out \
    || fail "F: pack --list-formats" f3.out

# G
"$MCPP" --protocol-version > g.out
for k in mcpp.build mcpp.launch mcpp.search mcpp.index mcpp.pack.formats; do
    grep -q "\"$k\"" g.out || fail "G: --protocol-version does not list $k" g.out
done

# H
mkdir -p "$TMP/h/src"
cd "$TMP/h"
printf '[package]\nname = "h"\nversion = "0.1.0"\nstandard = "c++23"\n' > mcpp.toml
for i in $(seq 1 40); do
    printf 'export module m%d;\nimport std;\nexport int f%d(){ std::vector<int> v(%d); return (int)v.size(); }\n' $i $i $i > "src/m$i.cppm"
done
{ echo 'import std;'; for i in $(seq 1 40); do echo "import m$i;"; done
  echo 'int main(){ std::println("{}", f1()+f40()); }'; } > src/main.cpp
"$MCPP" build > h0.log 2>&1 || fail "H: the first build failed" h0.log   # the std module in place
rm -rf target
"$MCPP" build > h.log 2>&1 &
pid=$!
for _ in $(seq 1 500); do
    grep -qs '"status": "running"' target/.last-result.json && break
    sleep 0.02
done
kill -TERM $pid
set +e
wait $pid; rc=$?
set -e
$PY - target/.last-result.json <<'EOF2' || fail "H: the interrupted build left no cancelled record (rc=$rc)" h.log target/.last-result.json
import json, sys
last = json.load(open(sys.argv[1]))["data"]
assert last["status"] == "cancelled", last
rec = json.load(open(last["result"]))["data"]
assert rec["status"] == "cancelled", rec["status"]
EOF2
[[ $rc -eq 143 ]] || fail "H: SIGTERM exited $rc, expected 143" h.log
"$MCPP" build > h2.log 2>&1 || fail "H: the build after the interrupted one failed" h2.log

echo "PASS: 936 the control contract"
