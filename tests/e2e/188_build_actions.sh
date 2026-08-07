#!/usr/bin/env bash
# requires: gcc
# 188_build_actions.sh — `mcpp:action=`: a build program DECLARES work instead
# of doing it, and the work becomes an edge in the build graph.
#
# The distinction this tests is the architectural one: a build program is a
# good place to decide what a build looks like (CONFIGURATION) and a bad place
# to perform it (WORK). Work done inside the program is serial, whole-set and
# reported as "build.mcpp exited 1". Declared as a node it is incremental,
# parallel and attributable to the edge that failed.
#
# All four wirings of the one primitive:
#   source   — outputs join the compile set, and are REGENERATED when an input
#              changes (the property the eager path can never have)
#   check    — outputs are a stamp; a failing check fails the build
#   object   — outputs join the LINK set (mcpp#365). Without it the link inputs
#              were the one attachment point a build graph obviously has and
#              this table could not express, so people routed around the graph
#              through `[build].ldflags` and lost incrementality.
#   artifact — inputs are link outputs, so ninja orders it after the link with
#              no phase machinery at all
#
# Also: a malformed action is refused rather than silently skipped.
#
# See .agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md §3.1.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src app/data
cd app

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF

cat > src/main.cpp <<'EOF'
#include <cstdio>
int generated_value();
int main() { std::printf("VALUE=%d\n", generated_value()); }
EOF

echo "11" > data/value.txt

# The "generator": reads data/value.txt, writes a .cpp returning that number.
cat > gen.sh <<'EOF'
#!/usr/bin/env bash
printf 'int generated_value() { return %s; }\n' "$(cat "$1")" > "$2"
EOF
chmod +x gen.sh

# A check that passes or fails depending on a marker file.
cat > check.sh <<'EOF'
#!/usr/bin/env bash
# The marker path is passed in: ninja runs commands with cwd = the BUILD dir,
# not the project root, so a bare relative name would never be found.
[ -f "$2" ] && { echo "check: refusing"; exit 1; }
: > "$1"
EOF
chmod +x check.sh

cat > pack.sh <<'EOF'
#!/usr/bin/env bash
# $1 = the linked binary, $2 = the artifact to produce
cp "$1" "$2"
EOF
chmod +x pack.sh

cat > build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/gen.cpp";

    mcpp::action a;
    a.id = "generate";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg((root + "/data/value.txt").c_str()).arg(out.c_str())
     .input((root + "/data/value.txt").c_str())
     .output(out.c_str())
     .submit();

    mcpp::action c;
    c.id = "lint";
    c.role = "check";
    c.arg((root + "/check.sh").c_str()).arg("${mcpp.out_dir}/lint.stamp")
     .arg((root + "/FAIL_THE_CHECK").c_str())
     .output("${mcpp.out_dir}/lint.stamp")
     .submit();

    mcpp::action p;
    p.id = "package";
    p.role = "artifact";
    p.arg((root + "/pack.sh").c_str()).arg("${mcpp.target_file:app}").arg("${mcpp.out_dir}/app.pack")
     .input("${mcpp.target_file:app}")
     .output("${mcpp.out_dir}/app.pack")
     .submit();
}
EOF

# ── 1. all three roles in one build ─────────────────────────────────────────
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build with actions failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^VALUE=' | tail -1)"
[[ "$out" == "VALUE=11" ]] || { echo "FAIL: generated source not linked: $out"; exit 1; }

OUTDIR=$(find target -name 'app.pack' -printf '%h\n' 2>/dev/null | head -1)
[ -n "$OUTDIR" ] || { cat b1.log; echo "FAIL: artifact action produced nothing"; exit 1; }
[ -f "$OUTDIR/lint.stamp" ] || { echo "FAIL: check action produced no stamp"; exit 1; }

# ── 2. incrementality — the property a pre-pass cannot have ────────────────
# Changing the generator's INPUT must regenerate, without build.mcpp re-running.
echo "23" > data/value.txt
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: rebuild failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^VALUE=' | tail -1)"
[[ "$out" == "VALUE=23" ]] || {
    cat b2.log; echo "FAIL: action did not re-run when its input changed: $out"; exit 1; }

# ...and an unrelated rebuild must NOT re-run it (that is the whole point).
touch src/main.cpp
"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: rebuild failed"; exit 1; }
grep -q "GENERATE" b3.log && {
    cat b3.log; echo "FAIL: the action re-ran although its inputs were unchanged"; exit 1; }

# ── 2b. changing the action's COMMAND also takes effect ────────────────────
# Distinct from 2: there the action's declared INPUT changed and ninja noticed.
# Here nothing ninja tracks changed — only build.mcpp, which re-runs (its own
# source is part of its cache key), re-declares the action with a different
# command, and ninja re-runs the edge because the rule's command changed. Both
# halves have to work or an edited generator invocation is silently ignored.
sed -i 's|"/data/value.txt"|"/data/other.txt"|g' build.mcpp
echo "31" > data/other.txt
"$MCPP" build > b2b.log 2>&1 || { cat b2b.log; echo "FAIL: rebuild after editing build.mcpp failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^VALUE=' | tail -1)"
[[ "$out" == "VALUE=31" ]] || {
    cat b2b.log; echo "FAIL: an edited action command did not take effect: $out"; exit 1; }

# ── 3. a failing check fails the build ─────────────────────────────────────
touch FAIL_THE_CHECK
rm -f "$OUTDIR/lint.stamp"
if "$MCPP" build > b4.log 2>&1; then
    cat b4.log; echo "FAIL: a failing check did not fail the build"; exit 1
fi
rm -f FAIL_THE_CHECK

# ── 2c. a companion output that is NOT a translation unit ──────────────────
# The single most natural generator shape: protoc emits foo.pb.cc AND foo.pb.h.
# Adopting every declared output into the compile set gave both the same object
# path and tripped "object path collision after uniqueness pass". The header
# must still be PRODUCED by the edge (things include it) but never compiled.
cat > genpair.sh <<'EOF'
#!/usr/bin/env bash
# $1 = .cc to write, $2 = .h to write
printf '#include "%s"\nint paired() { return 13; }\n' "$(basename "$2")" > "$1"
printf 'int paired();\n' > "$2"
EOF
chmod +x genpair.sh
cat >> src/main.cpp <<'EOF'
EOF
cat > build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action a;
    a.id = "pair"; a.role = "source";
    a.arg((root + "/genpair.sh").c_str())
     .arg((out + "/p.cc").c_str())
     .arg((out + "/p.h").c_str())
     .output((out + "/p.cc").c_str())
     .output((out + "/p.h").c_str())   // companion: produced, NOT compiled
     .submit();
    mcpp::include_dir(out.c_str());
    // Keep the earlier generated source in the build so main.cpp still links.
    mcpp::action g;
    g.id = "generate"; g.role = "source";
    g.arg((root + "/gen.sh").c_str()).arg((root + "/data/other.txt").c_str())
     .arg((out + "/gen.cpp").c_str())
     .input((root + "/data/other.txt").c_str())
     .output((out + "/gen.cpp").c_str())
     .submit();
}
EOF
rm -rf target
"$MCPP" build > b2c.log 2>&1 || {
    cat b2c.log; echo "FAIL: a companion (non-source) action output broke the build"; exit 1; }
grep -q "object path collision" b2c.log && {
    cat b2c.log; echo "FAIL: the header was adopted as a translation unit"; exit 1; }

# ── 3b/3c: their own minimal project ───────────────────────────────────────
# Separate from `app`, whose main.cpp deliberately depends on the generated
# symbol — swapping its build.mcpp out would fail the LINK and say nothing
# about what these two are actually testing.
mkdir -p "$TMP/edge/src"
cd "$TMP/edge"
cat > mcpp.toml <<'EOF'
[package]
name    = "edge"
version = "0.1.0"
EOF
printf 'int main() {}\n' > src/main.cpp

# ── 3b. a literal `$` in a command survives to the tool ────────────────────
# Shell-quoting alone does not save it: ninja expands `$foo` BEFORE the shell
# runs, so a token carrying a `$` (a path containing one, `-Wl,-rpath,$ORIGIN`,
# an awk program) needs ninja escaping too.
cat > dollar.sh <<'EOF'
#!/usr/bin/env bash
# $1 must arrive containing a literal dollar sign
case "$1" in *'$'*) : > "$2";; *) echo "lost the dollar: [$1]" >&2; exit 1;; esac
EOF
chmod +x dollar.sh
cat > build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    mcpp::action a;
    a.id = "dollar"; a.role = "check";
    a.arg((root + "/dollar.sh").c_str()).arg("-Wl,-rpath,$ORIGIN")
     .arg("${mcpp.out_dir}/dollar.stamp")
     .output("${mcpp.out_dir}/dollar.stamp")
     .submit();
}
EOF
rm -rf target
"$MCPP" build > b3b.log 2>&1 || {
    cat b3b.log; echo "FAIL: a literal \$ in an action command did not survive"; exit 1; }

# ── 3c. an unknown target reference is an error, not an empty path ─────────
cat > build.mcpp <<'EOF'
#include <cstdio>
import mcpp;
int main() {
    mcpp::action a;
    a.id = "bad-ref"; a.role = "artifact";
    a.arg("/bin/true").arg("${mcpp.target_file:no_such_target}")
     .input("${mcpp.target_file:no_such_target}")
     .output("${mcpp.out_dir}/x.out")
     .submit();
}
EOF
rm -rf target
if "$MCPP" build > b3c.log 2>&1; then
    cat b3c.log; echo "FAIL: an unknown target reference was accepted"; exit 1
fi
grep -q "no_such_target" b3c.log || {
    cat b3c.log; echo "FAIL: error does not name the unknown target"; exit 1; }

# ── 3d. role = "object": outputs join the LINK set ─────────────────────────
#
# The fourth wiring (mcpp#365). Source attaches to the compile inputs, Artifact
# to the link outputs, Check to nothing — which left the link INPUTS, an
# obvious attachment point, inexpressible. The consequence was concrete: a
# pre-built object could only reach the linker by being named in
# `[build].ldflags`, where it is a flat string in the command rather than a file
# in the graph, so editing it produced "ninja: no work to do".
#
# The object is produced by THE COMPILER MCPP ITSELF RESOLVED, read out of the
# generated build.ninja. Reaching for the host's `cc` looks simpler and is not:
# on a machine with xlings shims installed, a bare `cc` can resolve to a
# dispatcher pointing at some other sandbox, and the object would either fail to
# build or be built by a compiler whose ABI has nothing to do with the link.
mkdir -p "$TMP/objrole/src"
cd "$TMP/objrole"
cat > mcpp.toml <<'EOF'
[package]
name    = "objrole"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > src/main.cpp
"$MCPP" build > o0.log 2>&1 || { cat o0.log; echo "FAIL: probe build failed"; exit 1; }
OBJ_NINJA=$(find target -name build.ninja | head -1)
OBJ_CXX=$(sed -n 's/^cxx *= *//p' "$OBJ_NINJA" | head -1)
[ -n "$OBJ_CXX" ] || { cat "$OBJ_NINJA"; echo "FAIL: could not read the compiler out of build.ninja"; exit 1; }

cat > src/main.cpp <<'EOF'
#include <cstdio>
extern "C" int blob_value();
int main() { std::printf("BLOB=%d\n", blob_value()); return blob_value() == 7 ? 0 : 1; }
EOF
# extern "C" keeps the symbol unmangled without needing a C driver.
printf 'extern "C" int blob_value() { return 7; }\n' > blob.cpp
cat > mkobj.sh <<EOF
#!/usr/bin/env bash
# \$1 = source, \$2 = object to produce
"$OBJ_CXX" -c "\$1" -o "\$2"
EOF
chmod +x mkobj.sh
cat > build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action o;
    o.id = "blob"; o.role = "object";
    o.arg((root + "/mkobj.sh").c_str())
     .arg((root + "/blob.cpp").c_str())
     .arg((out + "/blob.o").c_str())
     .input((root + "/blob.cpp").c_str())
     .output((out + "/blob.o").c_str())
     .submit();
}
EOF
"$MCPP" build > o1.log 2>&1 || { cat o1.log; echo "FAIL: role=object build failed"; exit 1; }
"$MCPP" run > o2.log 2>&1 || { cat o2.log; echo "FAIL: the object was not linked in"; exit 1; }
grep -q 'BLOB=7' o2.log || { cat o2.log; echo "FAIL: wrong value from the linked object"; exit 1; }

# Tracked, unlike the ldflags workaround it replaces.
sleep 1
printf 'extern "C" int blob_value() { return 9; }\n' > blob.cpp
"$MCPP" run > o3.log 2>&1 && { cat o3.log; echo "FAIL: expected the changed object to be relinked (main asserts ==7)"; exit 1; }
grep -q 'BLOB=9' o3.log || { cat o3.log; echo "FAIL: editing the object's input did not reach the link"; exit 1; }
printf 'extern "C" int blob_value() { return 7; }\n' > blob.cpp

# An unknown target name is an error, not an edge that quietly attaches to
# nothing. (Artifact infers its target from ${mcpp.target_file:}; an object runs
# before the link and has to say the name.)
cat > build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action o;
    o.id = "blob"; o.role = "object";
    o.arg((root + "/mkobj.sh").c_str())
     .arg((root + "/blob.cpp").c_str())
     .arg((out + "/blob.o").c_str())
     .input((root + "/blob.cpp").c_str())
     .output((out + "/blob.o").c_str())
     .target("no_such_target")
     .submit();
}
EOF
rm -rf target
if "$MCPP" build > o4.log 2>&1; then
    cat o4.log; echo "FAIL: role=object accepted an unknown target"; exit 1
fi
grep -q "no_such_target" o4.log || {
    cat o4.log; echo "FAIL: error does not name the unknown target"; exit 1; }

# ...and EVERY name is checked, not just the case where none of them matched.
# A typo alongside a name that does exist is the shape a typo actually takes,
# and the check used to be gated on "nothing attached" — so `objrole` absorbed
# it and the misspelling was dropped without a word.
sed -i.bak 's/.target("no_such_target")/.target("objrole").target("no_such_target")/' build.mcpp
rm -f build.mcpp.bak
rm -rf target
if "$MCPP" build > o4b.log 2>&1; then
    cat o4b.log
    echo "FAIL: an unknown target was silently dropped because a sibling matched"; exit 1
fi
grep -q "no_such_target" o4b.log || {
    cat o4b.log; echo "FAIL: error does not name the unknown target"; exit 1; }

# ── 3e. the default target set includes TEST binaries ──────────────────────
#
# `mcpp build` linking and `mcpp test` failing on the very symbol the action
# exists to provide is the worst shape this can take: the object is what the
# library under test needs, and test link units are DISCOVERED from tests/*.cpp
# so their names cannot be spelled in build.mcpp without breaking plain
# `mcpp build`, where those units do not exist.
mkdir -p "$TMP/objtest/src" "$TMP/objtest/tests"
cd "$TMP/objtest"
cat > mcpp.toml <<'EOF'
[package]
name    = "objtest"
version = "0.1.0"
EOF
printf 'export module objtest.engine;\nextern "C" int blob_value();\nexport int get(){ return blob_value(); }\n' > src/engine.cppm
printf 'import objtest.engine;\nint main(){ return get()==7?0:1; }\n' > src/main.cpp
printf 'import objtest.engine;\nint main(){ return get()==7?0:1; }\n' > tests/t_engine.cpp
printf 'extern "C" int blob_value() { return 7; }\n' > blob.cpp
cp "$TMP/objrole/mkobj.sh" mkobj.sh
cat > build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action o;
    o.id = "blob"; o.role = "object";
    o.arg((root + "/mkobj.sh").c_str())
     .arg((root + "/blob.cpp").c_str())
     .arg((out + "/blob.o").c_str())
     .input((root + "/blob.cpp").c_str())
     .output((out + "/blob.o").c_str())
     .submit();                      // no .target(): every image, tests included
}
EOF
"$MCPP" build > o5.log 2>&1 || { cat o5.log; echo "FAIL: objtest build failed"; exit 1; }
"$MCPP" test  > o6.log 2>&1 || {
    cat o6.log
    echo "FAIL: the test binary did not receive the object (undefined symbol)"; exit 1; }
grep -q '1 passed' o6.log || { cat o6.log; echo "FAIL: the test did not run"; exit 1; }

# ── 3f. an object with no consumer at all is reported, not silent ──────────
#
# The edge is deliberately kept out of `actionDefaults` (an object's outputs are
# supposed to be reachable through a link edge), so "no consumer" means the
# command never runs. Saying nothing there is the same failure `[resources]`
# reports as resources/no-image.
mkdir -p "$TMP/objnone/src"
cd "$TMP/objnone"
cat > mcpp.toml <<'EOF'
[package]
name    = "objnone"
version = "0.1.0"

[targets.objnone]
kind = "lib"
EOF
printf 'export module objnone;\nexport int f(){return 1;}\n' > src/objnone.cppm
cp "$TMP/objtest/blob.cpp" blob.cpp
cp "$TMP/objtest/mkobj.sh" mkobj.sh
cp "$TMP/objtest/build.mcpp" build.mcpp
"$MCPP" build > o7.log 2>&1 || { cat o7.log; echo "FAIL: objnone build failed"; exit 1; }
grep -q 'no executable, shared library or test binary' o7.log || {
    cat o7.log; echo "FAIL: an object with no consumer must be reported"; exit 1; }

cd "$TMP/edge"

# ── 4. a malformed action is refused, not skipped ──────────────────────────
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    std::printf("mcpp:protocol=1\n");
    // No `command`, no `outputs` — mcpp fixes the source set during prepare,
    // so an output whose NAME is unknown cannot be built.
    std::printf("mcpp:action={\"id\":\"broken\",\"role\":\"source\"}\n");
}
EOF
rm -rf target
if "$MCPP" build > b5.log 2>&1; then
    cat b5.log; echo "FAIL: a malformed action was accepted"; exit 1
fi
grep -q "malformed action" b5.log || {
    cat b5.log; echo "FAIL: no diagnostic for the malformed action"; exit 1; }

echo "OK"
