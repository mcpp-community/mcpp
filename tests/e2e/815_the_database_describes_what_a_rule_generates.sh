#!/usr/bin/env bash
# requires: unix-shell python3
# 815_the_database_describes_what_a_rule_generates.sh — the S1 document names
# the files a build program's actions generate (S1 0.3.0 section 7.2, mcpp#724).
#
# `emit build-database` plans in a directory of its own and runs no action
# (SPEC-005 R2.1, R2.5), so a header an action generates is absent from the
# include directory the units' arguments name. The plan knows the generating
# step; the document states it, with the path a `mcpp build` of the same
# configuration writes. Criteria:
#   A. the package's set carries `ide.generated` entries: the header, with its
#      generator (id, inputs, arguments), and the generated include directory;
#      the package's test set, whose unit includes the same header, carries
#      the header too (SPEC-005 R3.12: every set of the package);
#   B. each entry's `build-path` is under the project's own `target/`, and a
#      following `mcpp build` writes the header at exactly that path;
#   C. compile_commands.json carries no such field.
set -e
# The project-root copy of the compile database is opt-in since
# 2026.10.26.1 ([build] compile_commands); this test reads it.
export MCPP_COMPILE_COMMANDS=compile_commands.json

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }
cd "$TMP"
mkdir -p proj/src proj/templates proj/tests
cd proj

cat > mcpp.toml <<'EOF'
[package]
name    = "gendb"
version = "0.1.0"
EOF
cat > templates/answer.h.in <<'EOF'
#pragma once
inline int generated_answer() { return 42; }
EOF
cat > src/main.cpp <<'EOF'
#include "answer.h"
int main() { return generated_answer() == 42 ? 0 : 1; }
EOF
cat > tests/test_answer.cpp <<'EOF'
#include "answer.h"
int main() { return generated_answer() == 42 ? 0 : 1; }
EOF
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    const std::string gen = std::string(mcpp::out_dir()) + "/gen";
    const std::string in  = std::string(mcpp::manifest_dir()) + "/templates/answer.h.in";
    const std::string out = gen + "/answer.h";
    mcpp::action a;
    a.id   = "gen:answer";
    a.role = mcpp::roles::source;
    a.arg("cp").arg(in.c_str()).arg(out.c_str())
     .input(in.c_str())
     .output(out.c_str())
     .submit();
    mcpp::include_dir(gen.c_str());
}
EOF

"$MCPP" emit build-database --format json > db.json 2> db.err || fail "the plan failed" db.err
python3 - "$PWD" > check.out 2>&1 <<'PY' || { cat check.out; exit 1; }
import json, os, sys
root = os.path.realpath(sys.argv[1])
env = json.load(open("db.json"))
sets = env["data"]["database"]["sets"]
gen = [g for s in sets for g in s.get("ide", {}).get("generated", [])]
headers = [g for g in gen if g["kind"] == "header"]
dirs = [g for g in gen if g["kind"] == "directory"]
assert headers, f"A: no generated header entry in {gen}"
h = headers[0]
assert h["path"].endswith(os.path.join("gen", "answer.h")), h
assert h["generator"]["id"] == "gen:answer", h
assert any(i.endswith("answer.h.in") for i in h["generator"]["inputs"]), h
assert h["generator"]["arguments"][0] == "cp", h
assert dirs and dirs[0]["path"].endswith("gen"), f"A: no generated directory entry in {gen}"
by_set = {s["name"]: s.get("ide", {}).get("generated", []) for s in sets}
assert "gendb:test" in by_set, f"A: no test set among {sorted(by_set)}"
for name in ("gendb", "gendb:test"):
    assert any(g["kind"] == "header" and g["generator"]["id"] == "gen:answer" for g in by_set[name]), \
        f"A: set {name} does not name the generated header: {by_set[name]}"
for g in (h, dirs[0]):
    bp = os.path.realpath(g["build-path"])
    assert bp.startswith(os.path.join(root, "target") + os.sep), f"B: {g['build-path']} is not under {root}/target"
open("build-path.txt", "w").write(h["build-path"])
print("ok")
PY
echo "ok: A. the set names the generated header, its step, and the generated directory"

"$MCPP" build > build.log 2>&1 || fail "the build failed" build.log
[ -f "$(cat build-path.txt)" ] || fail "B: the build did not write the header at the stated build-path" build.log
echo "ok: B. a build writes the header at the stated build-path"

"$MCPP" emit build-database --spec compile-commands > cdb.json 2> cdb.err || fail "the compile database failed" cdb.err
if grep -q '"generated"' cdb.json; then fail "C: compile_commands.json carries a generated field" cdb.json; fi
echo "ok: C. compile_commands.json carries no generated field"

echo "PASS: 815_the_database_describes_what_a_rule_generates"
