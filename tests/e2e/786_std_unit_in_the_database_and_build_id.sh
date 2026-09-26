#!/usr/bin/env bash
# requires: llvm python3
# 786_std_unit_in_the_database_and_build_id.sh — C5, D5a and D5b (design
# 2026-09-26
# .agents/docs/2026-09-26-compile-database-and-issue-699-design.md §3.5): a
# build that imports `std` lists the standard-library units in
# compile_commands.json, whose `directory` is the shared std cache, not the
# project's output directory. `emit --spec compile-commands` renders the same
# record, apart from the work directory. The S1 document's `provides` for
# `mcpp:std` names the BMI the build writes in that cache (not an empty
# string), and `ide.toolchains.<id>.build-id` is present and stable across two
# runs (S1 §6, S1-11.2-3).
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
mkdir -p app/src
cd app
cat > mcpp.toml <<'EOF'
[package]
name = "stdunit"
version = "0.1.0"
standard = "c++23"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("hi"); return 0; }
EOF

"$MCPP" build --toolchain llvm@22.1.8 > build.log 2>&1 || {
    cat build.log; echo "FAIL: build failed"; exit 1; }

python3 - <<'PY'
import json, os, sys

entries = json.load(open("compile_commands.json"))
std = [e for e in entries if os.path.basename(e["file"]) == "std.cppm"]
assert len(std) == 1, [e["file"] for e in entries]
std = std[0]
project_target = os.path.join(os.getcwd(), "target")
assert not std["directory"].startswith(project_target), std["directory"]
assert os.path.isdir(std["directory"]), std["directory"]
print(f"  (std directory: {std['directory']})")
print("ok: the std unit's directory is the shared std cache, not the project's output directory")
PY

"$MCPP" emit build-database --toolchain llvm@22.1.8 --spec compile-commands \
    > emitted.json 2> emit.err || { cat emit.err; echo "FAIL: emit failed"; exit 1; }

python3 - <<'PY'
import json

built = {e["file"]: e for e in json.load(open("compile_commands.json"))}
emitted = {e["file"]: e for e in json.load(open("emitted.json"))}
std_file = next(f for f in built if f.endswith("std.cppm"))
b, e = built[std_file], emitted[std_file]
# The only difference by construction is where a PROJECT unit's work
# directory points (emit plans under the mcpp home, the build under the
# project); the standard-library units live in the shared cache regardless,
# so their record is identical in both documents.
assert b["arguments"] == e["arguments"], (b["arguments"], e["arguments"])
assert b["directory"] == e["directory"], (b["directory"], e["directory"])
assert b["output"] == e["output"], (b["output"], e["output"])
print("ok: emit --spec compile-commands renders the same std entry as the build's database")
PY

"$MCPP" emit build-database --toolchain llvm@22.1.8 --format json > s1_1.json 2> s1_1.err \
    || { cat s1_1.err; echo "FAIL: emit (s1, run 1) failed"; exit 1; }
"$MCPP" emit build-database --toolchain llvm@22.1.8 --format json > s1_2.json 2> s1_2.err \
    || { cat s1_2.err; echo "FAIL: emit (s1, run 2) failed"; exit 1; }

python3 - <<'PY'
import json

def load(p):
    return json.load(open(p))["data"]["database"]

d1, d2 = load("s1_1.json"), load("s1_2.json")

def std_provides(d):
    for s in d["sets"]:
        if s["name"] != "mcpp:std":
            continue
        for u in s["translation-units"]:
            if "std" in u["provides"]:
                return u["provides"]["std"]
    raise AssertionError("no mcpp:std unit providing 'std'")

path1 = std_provides(d1)
assert path1, "provides['std'] is empty; D5b names the BMI path"
assert path1.endswith(".pcm") or path1.endswith(".gcm") or path1.endswith(".ifc"), path1
assert path1 == std_provides(d2), (path1, std_provides(d2))
print(f"  (provides['std']: {path1})")
print("ok: provides['std'] names the std-cache BMI")

tc1 = list(d1["ide"]["toolchains"].values())[0]
tc2 = list(d2["ide"]["toolchains"].values())[0]
bid1, bid2 = tc1.get("build-id"), tc2.get("build-id")
assert bid1, "build-id is absent"
assert bid1 == bid2, (bid1, bid2)
print(f"  (build-id: {bid1})")
print("ok: build-id is present and stable across two runs")
PY

echo "PASS: 786 the std unit in the database, emit/build agreement, provides and build-id"
