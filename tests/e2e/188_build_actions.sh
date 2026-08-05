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
# All three wirings of the one primitive:
#   source   — outputs join the compile set, and are REGENERATED when an input
#              changes (the property the eager path can never have)
#   check    — outputs are a stamp; a failing check fails the build
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

# ── 3. a failing check fails the build ─────────────────────────────────────
touch FAIL_THE_CHECK
rm -f "$OUTDIR/lint.stamp"
if "$MCPP" build > b4.log 2>&1; then
    cat b4.log; echo "FAIL: a failing check did not fail the build"; exit 1
fi
rm -f FAIL_THE_CHECK

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
