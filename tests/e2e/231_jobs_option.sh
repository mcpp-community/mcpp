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
