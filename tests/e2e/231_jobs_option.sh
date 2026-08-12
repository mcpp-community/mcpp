#!/usr/bin/env bash
# `--jobs N|auto` and the `--` boundary that keeps it from eating a program's flags.
#
# Two separate contracts, both easy to break without noticing:
#   1. the option is honoured and a bad value is REPORTED, not silently dropped
#      (a typo that quietly restores the default is a build mysteriously slower
#      than the user asked for);
#   2. `-j` is a common enough flag on other programs that `mcpp run -- -j 4`
#      must reach the child untouched. `--jobs` reaches its consumer through the
#      MCPP_JOBS side channel, and that pre-scan used to walk the whole argv.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

cat > mcpp.toml <<'EOF'
[package]
name    = "jobsopt"
version = "0.1.0"

[toolchain]
default = "gcc@16.1.0"
macos   = "llvm@22.1.8"
windows = "llvm@20.1.7"
EOF
mkdir -p src
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) std::printf("%s\n", argv[i]);
}
EOF

# 1. A numeric value builds.
"$MCPP" build --release --jobs 2 > "$TMP/j2.txt" 2>&1 \
  || { echo "--jobs 2 failed:"; cat "$TMP/j2.txt"; exit 1; }

# 2. `auto` builds too. Its value depends on the host, so the assertion is that
#    it is ACCEPTED — asserting a particular number would encode this machine.
"$MCPP" build --release --jobs auto > "$TMP/jauto.txt" 2>&1 \
  || { echo "--jobs auto failed:"; cat "$TMP/jauto.txt"; exit 1; }
if grep -qi 'invalid job count' "$TMP/jauto.txt"; then
    echo "'auto' was rejected as an invalid job count:"; cat "$TMP/jauto.txt"; exit 1
fi

# 3. A bad value must WARN and still build (degrading to the backend default).
#    Asserted from both sides: a silent drop and a hard failure are both wrong.
"$MCPP" build --release --jobs bogus > "$TMP/jbad.txt" 2>&1 \
  || { echo "a bad --jobs value should warn, not fail the build:"; cat "$TMP/jbad.txt"; exit 1; }
grep -qi 'invalid job count' "$TMP/jbad.txt" \
  || { echo "a bad --jobs value was accepted silently:"; cat "$TMP/jbad.txt"; exit 1; }

# 4. THE BOUNDARY. Everything after `--` belongs to the program.
"$MCPP" run -- -j bogus > "$TMP/sep.txt" 2>&1 \
  || { echo "run with trailing program args failed:"; cat "$TMP/sep.txt"; exit 1; }
grep -qx -- '-j'    "$TMP/sep.txt" || { echo "'-j' did not reach the program:";    cat "$TMP/sep.txt"; exit 1; }
grep -qx -- 'bogus' "$TMP/sep.txt" || { echo "'bogus' did not reach the program:"; cat "$TMP/sep.txt"; exit 1; }
# ...and mcpp must not have interpreted it as its own concurrency setting.
if grep -qi 'invalid job count' "$TMP/sep.txt"; then
    echo "mcpp consumed a flag that belonged to the program:"; cat "$TMP/sep.txt"; exit 1
fi

echo "jobs option OK"

# 10. `--toolchain` selects a toolchain for ONE build, without touching the
#     manifest. This is the usable form of "which compiler": on mcpp itself the
#     choice is worth 2.5x (gcc 81.8s vs llvm 32.6s), but changing the DEFAULT
#     would invalidate every published package's fingerprint, so per-build
#     selection is the part that costs nobody anything.
#
#     Asserted by OBSERVING THE RESOLUTION, not by timing: a timing assertion on
#     CI measures the runner's mood.
#     The spec comes from THIS platform's own resolution, not a hard-coded
#     `gcc@16.1.0`: the fixture pins llvm on macOS and Windows, and asserting a
#     compiler that is not installed there tests the payload index, not the flag.
own=$("$MCPP" build --release 2>&1 | sed -n 's/.*Resolved \([^ ]*\) .*/\1/p' | head -1)
[ -n "$own" ] || { echo "could not learn this platform's toolchain"; exit 1; }
out=$("$MCPP" build --release --toolchain "$own" 2>&1) \
  || { echo "--toolchain $own failed:"; echo "$out"; exit 1; }
echo "$out" | grep -q "Resolved $own" \
  || { echo "--toolchain did not reach toolchain resolution:"; echo "$out"; exit 1; }

# ...and it must BEAT the manifest, or it is not an override. The fixture pins
# gcc on Linux, so asking for something else has to change what gets resolved.
if [ "$(uname -s)" = "Linux" ]; then
    out=$("$MCPP" build --release --toolchain llvm@22.1.8 2>&1) || true
    echo "$out" | grep -q 'Resolved llvm@22.1.8' \
      || { echo "--toolchain lost to the manifest pin:"; echo "$out"; exit 1; }
fi

echo "toolchain override OK"

# 11. The split module schedule (L2). Default is OFF; `on` selects it. Asserted
#     on the GRAPH's own declaration and on the edges it emits — not on timing,
#     which on CI measures the runner's mood.
"$MCPP" build --release > /dev/null 2>&1
ninja_file=$(find target -name build.ninja | head -1)
grep -q 'schedule=none' "$ninja_file" \
  || { echo "default should not use the split schedule:"; head -2 "$ninja_file"; exit 1; }

rm -rf target
MCPP_BMI_SCHEDULE=on "$MCPP" build --release > "$TMP/sched.txt" 2>&1 \
  || { echo "schedule=on failed to build:"; cat "$TMP/sched.txt"; exit 1; }
ninja_file=$(find target -name build.ninja | head -1)
grep -q 'schedule=detach-codegen\|schedule=two-phase\|schedule=none' "$ninja_file" \
  || { echo "graph does not declare its schedule:"; head -2 "$ninja_file"; exit 1; }

# The no-op check is GATED on the split shape actually being in effect. It
# exists to catch one specific defect — a depfile whose target does not match
# the edge's output, which looks exactly like success while recompiling
# everything — and that defect only exists where BMI edges do. Asserting it
# where the graph is ordinary measures unrelated platform behaviour instead:
# on macOS `on` selects two-phase, which the backend does not emit, and the
# check failed on one object rebuilt for reasons that predate this feature.
# The reference mark is taken AFTER the first build, not from its stdout
# redirect: that file's mtime is when the shell opened it, which is before the
# objects exist, so every object counted as "newer" and the comparison measured
# nothing but timestamp ordering.
if grep -q 'schedule=detach-codegen' "$ninja_file"; then
    sleep 1
    touch "$TMP/mark"
    MCPP_BMI_SCHEDULE=on "$MCPP" build --release > /dev/null 2>&1
    rebuilt=$(find target -name '*.o' -newer "$TMP/mark" | wc -l)
    [ "$rebuilt" -eq 0 ] \
      || { echo "second build under the split schedule recompiled $rebuilt object(s)"; exit 1; }
fi

echo "split schedule OK"
