#!/usr/bin/env bash
# requires: python3
# 736 -- an element of a compile-flag list stands for the same words on every
# host, the compiler receives those words, and both compile databases list
# them (#655).
#
# Every expectation below is the same on Linux, macOS and Windows: the words
# are defined by the manifest's flag syntax, not by the host's command-line
# reader. Criteria:
#   A. the program prints the value each macro received;
#   B. compile_commands.json lists the words for the unit;
#   C. `mcpp emit build-database` lists the same words for the unit;
#   D. the first plan names the elements whose words changed in 2026.9.17.1,
#      and does not name the ones whose words did not;
#   E. a build that repeats the plan names nothing.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
# The TOML below is written with a quoted heredoc, so every backslash is the
# manifest's own. Each element is followed by the words it stands for.
cat > mcpp.toml <<'EOF'
[package]
name    = "flagwords"
version = "0.1.0"

[build]
cxxflags = [
  "-DV_ESC=\\\"esc.h\\\"",      # -DV_ESC="esc.h"   (the libarchive spelling)
  "-DV_MID=\"mid\"",            # -DV_MID=mid
  "-DV_SQ='sq'",                # -DV_SQ=sq
  "'-DV_SP=a b'",               # -DV_SP=a b
  "-DV_DOL=a$b",                # -DV_DOL=a$b
  "-O0 -DV_A=1",                # -O0  -DV_A=1
  "-DV_B=long long",            # -DV_B=long long   (a -D element with a space
                                #  is one word, verbatim: mcpp#234)
]
defines = [
  "V_DEF=\"def\"",              # -DV_DEF="def"
  "V_DSP=a b",                  # -DV_DSP=a b
]
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
#define STR2(...) #__VA_ARGS__
#define STR(...) STR2(__VA_ARGS__)
static void show(const char* name, const char* value) {
    std::printf("%s=[%s]\n", name, value);
}
int main() {
    show("V_ESC", STR(V_ESC));
    show("V_MID", STR(V_MID));
    show("V_SQ",  STR(V_SQ));
    show("V_SP",  STR(V_SP));
    show("V_DOL", STR(V_DOL));
    show("V_A",   STR(V_A));
    show("V_B",   STR(V_B));
    show("V_DEF", STR(V_DEF));
    show("V_DSP", STR(V_DSP));
    return 0;
}
EOF

"$MCPP" build > build1.log 2>&1 || fail "the first build failed" build1.log

# A. What the compiler received.
"$MCPP" run > run.log 2>&1 || fail "the program did not run" run.log
tr -d '\r' < run.log > run.txt
cat > want.txt <<'EOF'
V_ESC=["esc.h"]
V_MID=[mid]
V_SQ=[sq]
V_SP=[a b]
V_DOL=[a$b]
V_A=[1]
V_B=[long long]
V_DEF=["def"]
V_DSP=[a b]
EOF
for line in $(seq 1 9); do
    want=$(sed -n "${line}p" want.txt)
    grep -qxF -- "$want" run.txt || fail "A: the program did not print $want" run.txt
done

# B and C. What the databases list. The expected words are the ones above.
cat > words.json <<'EOF'
["-DV_ESC=\"esc.h\"", "-DV_MID=mid", "-DV_SQ=sq", "-DV_SP=a b",
 "-DV_DOL=a$b", "-O0", "-DV_A=1", "-DV_B=long long", "-DV_DEF=\"def\"", "-DV_DSP=a b"]
EOF
cdb=$(find target -name compile_commands.json | head -1)
[ -n "$cdb" ] || cdb=compile_commands.json
[ -f "$cdb" ] || fail "B: no compile_commands.json"
"$MCPP" emit build-database --format json > db.json 2> db.err || fail "C: emit failed" db.err
python3 - "$cdb" db.json words.json <<'PY' || exit 1
import json, sys
words = json.load(open(sys.argv[3]))

def contains(args):
    return all(w in args for w in words) and not any(
        "\\\"" in a or a.endswith("'") for a in args if a.startswith("-DV_"))

entries = json.load(open(sys.argv[1]))
main = [e for e in entries if e["file"].replace("\\", "/").endswith("src/main.cpp")]
if not main or not contains(main[0]["arguments"]):
    print("FAIL: B: compile_commands.json does not list the words")
    print(json.dumps(main[0]["arguments"] if main else entries, indent=1))
    sys.exit(1)

def lists(node):
    if isinstance(node, dict):
        for v in node.values():
            yield from lists(v)
    elif isinstance(node, list):
        if node and all(isinstance(x, str) for x in node):
            yield node
        for v in node:
            yield from lists(v)

doc = json.load(open(sys.argv[2]))
found = []
for args in lists(doc):
    if any(a.startswith("-DV_") for a in args):
        found.append(args)
# S1 may split a unit's arguments into a set's baseline and the unit's own
# local arguments; the words must be present across the lists that carry them.
flat = [a for args in found for a in args]
if not contains(flat):
    print("FAIL: C: the build database does not list the words")
    print(json.dumps(found, indent=1))
    sys.exit(1)
PY

# D. The first plan names the elements whose words changed, and only those.
for element in "V_DEF=\"def\"" "-DV_DOL=a\$b"; do
    grep -qF -- "element '$element' reaches the compiler as" build1.log \
        || fail "D: the first plan does not name $element" build1.log
done
for element in "-DV_MID=\"mid\"" "V_DSP=a b" "-DV_ESC=" "-DV_B=long long" "-O0 -DV_A=1"; do
    if grep -F -- "reaches the compiler as" build1.log | grep -qF -- "element '$element"; then
        fail "D: the first plan names $element, whose words did not change" build1.log
    fi
done

# E. A repeated plan names nothing.
touch src/main.cpp
"$MCPP" build > build2.log 2>&1 || fail "the second build failed" build2.log
if grep -q "reaches the compiler as" build2.log; then
    fail "E: a repeated plan repeated the notes" build2.log
fi

echo "OK"
