#!/usr/bin/env bash
# requires: gcc
# A MODULE-EXTENSION FILE NEED NOT PROVIDE A MODULE.
#
# An implementation unit (`module M;`) is a legal inhabitant of a `.cppm`, and
# it provides nothing importable. mcpp answers "is this a module interface"
# twice: the CLASSIFIER answers from the extension, and the SCANNER answers from
# the content. `pick_rule` read the first and the BMI binding read the second,
# so on a file where they disagree the edge carried an interface's flags with no
# BMI to put anywhere.
#
# THE CRITERION IS THE GRAPH, AND IT HAS TO BE. On GCC this project built before
# the fix and builds after it, because GCC's interface spelling is the plain
# language and GCC needs no explicit BMI path -- so a build-outcome assertion
# would pass in both worlds and measure nothing. What the two worlds disagree
# about is what mcpp EMITS: every module edge now states which of the two it is.
# The compiler that made the defect visible is clang, whose driver infers
# `c++-module` from the extension and rejects the implementation unit outright;
# the assertion below holds on both, which is the reason it is written this way.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src

cat > src/foo.cppm <<'EOF'
export module foo;
export int foo();
EOF

# The same extension, and not an interface.
cat > src/foo_impl.cppm <<'EOF'
module foo;
int foo() { return 5; }
EOF

cat > src/main.cpp <<'EOF'
import foo;
int main() { return foo() == 5 ? 0 : 1; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "implunit"
version = "0.1.0"
EOF

"${MCPP:-mcpp}" build > build.log 2>&1 || {
    echo "FAIL: an implementation unit in a .cppm did not build"
    cat build.log
    exit 1; }

bin=$(find target -type f -name implunit -perm -u+x | head -1)
[ -n "$bin" ] || { echo "FAIL: no binary"; exit 1; }
"$bin" || { echo "FAIL: the program did not return 5"; exit 1; }

graph=$(find target -name build.ninja | head -1)
[ -n "$graph" ] || { echo "FAIL: no build graph"; exit 1; }

# EVERY module edge states its language, AND THE DENOMINATOR IS ASSERTED.
# A ratio check whose denominator is zero reads exactly like a pass.
edges=$(grep -c ' : cxx_module ' "$graph" || true)
langs=$(grep -c '^  module_lang =' "$graph" || true)
[ "$edges" -ge 2 ] || {
    echo "FAIL: expected at least 2 module edges, found $edges"
    echo "      (this fixture no longer exercises the question)"
    grep -n 'cxx_module' "$graph" || true
    exit 1; }
[ "$edges" = "$langs" ] || {
    echo "FAIL: $edges module edges but $langs stated a language"
    echo "      an edge with no module_lang takes whatever the driver infers"
    echo "      from the extension, which is the defect this test exists for"
    exit 1; }

# The empty-value spelling must not appear. On clang this was accepted, exited
# 0, and wrote no BMI at all.
grep -qE -- '-fmodule-output=(\s|$)' "$graph" && {
    echo "FAIL: an empty -fmodule-output= reached the graph"
    grep -n -- '-fmodule-output=' "$graph"
    exit 1; }

echo "ok: $edges module edges, all $langs of them stated"
