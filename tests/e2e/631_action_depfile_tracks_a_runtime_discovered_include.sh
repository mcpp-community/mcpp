#!/usr/bin/env bash
# requires: gcc
# 631_action_depfile_tracks_a_runtime_discovered_include.sh — `mcpp::action`'s
# `depfile` field: a rule that discovers its own dependency graph by RUNNING
# (glslangValidator `--depfile`, glslc `-MD -MF`, slangc `-depfile`, nvcc/clang
# `-MD -MF`) has a way to tell ninja about it.
#
# THE DEFECT THIS DEFENDS AGAINST. `a.inputs` is fixed when build.mcpp runs,
# before the action's own command has executed. A shader/device compiler does
# not know its `#include` graph at that point — it learns it by PARSING the
# source, and reports the result afterward as a Make-style depfile. Without
# this field there is no channel for that report to reach ninja: the action's
# edge tracks exactly the files build.mcpp named, so editing a file the
# command merely READ (never a declared `.input()`) reruns nothing, and
# `mcpp build` stays green over a stale generated artifact.
#
# THE SCENARIO. The action's command is a stand-in generator: it writes its
# declared output AND a depfile naming a second file — `included.glsl` — that
# the action never declares as an input. Touching that second file (a bare
# `touch`, no content edit — exactly what a real `#include`'s mtime changing
# looks like) must make ninja rerun the action on the next build.
#
# ASSERTED ON A RUN COUNTER gen.sh writes as a side effect, NOT on grepping
# `mcpp build`'s own log for the ninja description text ("GENERATE ..."). That
# text cannot appear there: on a non-verbose SUCCESSFUL build mcpp passes
# ninja `--quiet` and only surfaces its captured stdout when the build FAILS
# or `--verbose` is given (src/build/ninja_backend.cppm, src/build/execute.cppm)
# — so a log grep for it on success is vacuous, true whether or not the edge
# ran. Verified directly against this fixture: even the very first, from-
# scratch build (which unquestionably runs the action) prints no such line.
# The run counter is a real side effect of the COMMAND executing, so reading
# it back is a direct measurement instead of a guess about log formatting.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
mkdir -p "$TMP/shaderdep/src"
cd "$TMP/shaderdep"

cat > mcpp.toml <<'EOF'
[package]
name    = "shaderdep"
version = "0.1.0"
EOF

cat > src/main.cpp <<'EOF'
#include <cstdio>
int generated_value();
int main() { std::printf("VALUE=%d\n", generated_value()); }
EOF

# The file the ACTION discovers only by "compiling" — analogous to a shader's
# `#include`. Its CONTENT is never read below; only its mtime matters, which
# is exactly what a depfile-tracked dependency promises to react to.
echo "float unused = 1.0;" > included.glsl

# The stand-in device compiler. $1 = the discovered file, $2 = the source it
# generates, $3 = the depfile to write, $4 = a run counter so the test can
# tell the command executed again versus ninja deciding it was up to date.
# Deliberately NOT `set -e`: the `[ -f ] &&` idiom below is a normal false
# branch, not an error.
cat > gen.sh <<'EOF'
#!/usr/bin/env bash
n=0
[ -f "$4" ] && n="$(cat "$4")"
n=$((n + 1))
echo "$n" > "$4"
printf 'int generated_value() { return %s; }\n' "$n" > "$2"
# Make syntax, the shape `-MD -MF`/`--depfile` produce: OUTPUT: PREREQUISITES.
printf '%s: %s\n' "$2" "$1" > "$3"
EOF
chmod +x gen.sh

cat > build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root  = mcpp::manifest_dir();
    const std::string out   = std::string(mcpp::out_dir()) + "/generated.cpp";
    const std::string dep   = std::string(mcpp::out_dir()) + "/generated.cpp.d";
    const std::string count = root + "/run_count.txt";

    mcpp::action a;
    a.id      = "genshader";
    a.role    = "source";
    a.depfile = dep.c_str();
    // Deliberately NOT declared as `.input(...)`: the whole point is that
    // ninja learns about it from the depfile the command writes, not from
    // anything build.mcpp told the engine in advance.
    a.arg((root + "/gen.sh").c_str())
     .arg((root + "/included.glsl").c_str())
     .arg(out.c_str())
     .arg(dep.c_str())
     .arg(count.c_str())
     .output(out.c_str())
     .submit();
}
EOF

run_count() { cat run_count.txt 2>/dev/null || echo "<missing>"; }

# ── 1. first build: the action runs once, VALUE reflects run #1 ────────────
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: initial build failed"; exit 1; }
[[ "$(run_count)" == "1" ]] || {
    cat b1.log; echo "FAIL: the action did not run on a fresh build (count=$(run_count))"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^VALUE=' | tail -1)"
[[ "$out" == "VALUE=1" ]] || { echo "FAIL: unexpected initial output: $out"; exit 1; }

# ── 2. THE CONTROL: rebuild with nothing changed must not rerun the action ──
# Without this half, the assertion in part 3 would pass against a backend
# that reruns the action on every build regardless of the depfile.
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: no-op rebuild failed"; exit 1; }
[[ "$(run_count)" == "1" ]] || {
    cat b2.log
    echo "FAIL: the action reran although nothing it tracks changed (count=$(run_count))"
    exit 1; }

# ── 3. touch the file named ONLY in the depfile — never a declared input ───
touch included.glsl
"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: rebuild after touch failed"; exit 1; }
[[ "$(run_count)" == "2" ]] || {
    cat b3.log
    echo "FAIL: touching a file the depfile named did not rerun the action"
    echo "      (count=$(run_count), expected 2)"
    echo "      this is the defect: a.inputs alone cannot express this"
    echo "      dependency, and without depfile support ninja never learns it"
    exit 1; }
out="$("$MCPP" run 2>&1 | grep '^VALUE=' | tail -1)"
[[ "$out" == "VALUE=2" ]] || {
    echo "FAIL: the action reran but the artifact was not regenerated: $out"
    echo "      (expected VALUE=2)"
    exit 1; }

# ── 4. and it behaves once more, so #3 was not a first-build artifact ──────
touch included.glsl
"$MCPP" build > b4.log 2>&1 || { cat b4.log; echo "FAIL: second touch rebuild failed"; exit 1; }
[[ "$(run_count)" == "3" ]] || {
    cat b4.log
    echo "FAIL: the second touch did not rerun the action (count=$(run_count))"
    exit 1; }
out="$("$MCPP" run 2>&1 | grep '^VALUE=' | tail -1)"
[[ "$out" == "VALUE=3" ]] || {
    echo "FAIL: unexpected output after the second touch: $out (expected VALUE=3)"
    exit 1; }

echo "OK"
