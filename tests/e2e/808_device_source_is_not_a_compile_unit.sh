#!/usr/bin/env bash
# requires: gcc
# 808 -- a rule-claimed device source is not a C++ compile unit of the plan
# (mcpp#724 §1, design 2026-09-27 §4.1).
#
# `SourceKind::Device` names a file the engine has no compile rule for: it is
# compiled, if at all, by the package's build program through an action, never
# by a `cxx_object` edge. Until now the plan turned every graph unit into a
# `CompileUnit`, device units included, so `build.ninja` carried a dead
# `cxx_object` edge for the device source and both `compile_commands.json` and
# the S1 document (`emit build-database`) listed it with a compiler command
# that never ran on the file. Criteria, on examples/12's own fixture (a `.toy`
# kernel a rule claims and turns into generated C++):
#   A. `build.ninja` has no `cxx_object` edge whose input is the device source.
#   B. `compile_commands.json` lists no entry for it.
#   C. The S1 document (`emit build-database --format json`) lists no unit for
#      it, in any set.
#   D. `--spec compile-commands` agrees with B.
#   E. The build still succeeds and the kernel still reaches the program: the
#      fix must not stop the device source from being compiled BY THE RULE.
set -e

SRC="$(cd "$(dirname "$0")/../.." && pwd)/examples/12-a-new-device-language"
[[ -d "$SRC" ]] || { echo "FAIL: $SRC is missing"; exit 1; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cp -r "$SRC" "$TMP/ex"
find "$TMP/ex" -maxdepth 3 -type d -name target -exec rm -rf {} + 2>/dev/null || true
find "$TMP/ex" -maxdepth 3 -name mcpp.lock -delete 2>/dev/null || true

cd "$TMP/ex/app"

# ── A, B: build.ninja and compile_commands.json ─────────────────────────────
"$MCPP" build --configure-only > configure.log 2>&1 \
    || { cat configure.log; echo "FAIL: configure-only did not succeed"; exit 1; }

ninja_file="$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)"
[[ -n "$ninja_file" ]] || { echo "FAIL: no build.ninja generated"; exit 1; }

if grep -qE '^build [^:]*: cxx_object [^|]*answer\.toy' "$ninja_file"; then
    grep -nE '^build [^:]*: cxx_object [^|]*answer\.toy' "$ninja_file"
    echo "FAIL: A: build.ninja carries a cxx_object edge for the device source"
    exit 1
fi
echo "ok: A, no cxx_object edge for the device source"

[[ -f compile_commands.json ]] || { echo "FAIL: no compile_commands.json"; exit 1; }
if grep -q 'answer\.toy' compile_commands.json; then
    echo "FAIL: B: compile_commands.json lists the device source"
    exit 1
fi
echo "ok: B, compile_commands.json lists no entry for the device source"

# ── C: the S1 document ───────────────────────────────────────────────────
"$MCPP" emit build-database --format json > s1.json 2> s1.err \
    || { cat s1.err; echo "FAIL: emit build-database failed"; exit 1; }
python3 - s1.json <<'EOF' || { echo "FAIL: C"; exit 1; }
import json, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
for s in db["sets"]:
    for u in s["translation-units"]:
        if u["source"].endswith("answer.toy"):
            print("device source listed in set", s["name"], u["source"])
            sys.exit(1)
EOF
echo "ok: C, the S1 document lists no unit for the device source"

# ── D: --spec compile-commands ───────────────────────────────────────────
"$MCPP" emit build-database --spec compile-commands > cc.json 2> cc.err \
    || { cat cc.err; echo "FAIL: emit --spec compile-commands failed"; exit 1; }
if grep -q 'answer\.toy' cc.json; then
    echo "FAIL: D: --spec compile-commands lists the device source"
    exit 1
fi
echo "ok: D, --spec compile-commands agrees"

# ── E: the fix must not stop the rule from compiling the kernel ────────────
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "FAIL: E: the build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == *"= 42"* ]] || { echo "FAIL: E: the kernel did not reach the program: '$out'"; exit 1; }
echo "ok: E, the device source is still compiled by the rule and the build still runs"

echo "PASS: 808 a device source is not a compile unit"
