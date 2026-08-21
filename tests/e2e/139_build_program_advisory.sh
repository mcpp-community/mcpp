#!/usr/bin/env bash
# requires: unix-shell
# A build program that succeeds and still says something.
#
# ⚠️ THE SECOND BUILD IS THE TEST, NOT THE FIRST.
#
# mcpp prints what it captured from a build program only when that program
# EXITS NON-ZERO, which is why `mcpp:warning=` had to exist at all. But the
# result of a build program is CACHED, and a cache hit does not re-run it — so
# an implementation that emitted the advisory only where the program was
# actually executed would print it on a project's first build and never again.
#
# That failure mode is worse than having no channel: an advisory that stops
# appearing reads as "the condition was resolved". It is also invisible to any
# test that builds once, which is what a test written the obvious way does.
#
# So the assertions below are ordered: first build, then a SECOND build that
# must be a cache hit and must still carry the same line.
#
# ⚠️ This test was confirmed to FAIL before the fix — the cache-hit emission
# was removed and the second-build assertion went red while the first stayed
# green. A test for a replay path that has never been observed failing cannot
# distinguish "replay works" from "the cache never hit".
# ⚠️ `"$MCPP"`, NEVER A BARE `mcpp`. The harness passes the binary under test;
# a bare name resolves through PATH to whichever engine happens to be installed,
# and this test would then pass against an engine that does not have the feature.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p p/src
cd p

cat > mcpp.toml <<'EOF'
[package]
name    = "advisory"
version = "0.1.0"
EOF

cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF

# The realistic shape: the program looked for something optional, did not find
# it, configured nothing that depends on it, and says so. It exits 0 — the
# build is correct, the user is merely missing a capability.
cat > build.mcpp <<'EOF'
import mcpp;
import std;
int main() {
    mcpp::warning("no emulator found; `xlings install qemu-riscv -y` enables `mcpp run`");
    mcpp::define("ADVISORY_RAN");
    return 0;
}
EOF

# ── 1. The advisory reaches the user on a build that SUCCEEDED ──────────────
"$MCPP" build > first.log 2>&1
grep -q "no emulator found" first.log \
  || { cat first.log; echo "FAIL: the advisory did not reach the output"; exit 1; }

# ⚠️ Prefixed with the package it came from. A workspace has several build
# programs and an unattributed sentence sends the reader to the wrong manifest.
grep -q "advisory: no emulator found" first.log \
  || { cat first.log; echo "FAIL: the advisory was not attributed to its package"; exit 1; }

# The build succeeded. An advisory is not an error.
grep -qi "Finished" first.log \
  || { cat first.log; echo "FAIL: the build did not finish"; exit 1; }

# ── 2. ⭐ THE SECOND BUILD. The program does not run; the advisory must ─────
#
# ⚠️ `touch` FIRST, AND NOT AS A SUPERSTITION. A build with nothing at all to
# do takes a WHOLE-PROJECT fast path that prints one line and never reaches the
# build.mcpp stage — so an unmodified second build exercises neither the run
# path nor the replay path, and asserting against it would test the fast path
# instead. This repository has been bitten by that masking before.
#
# Touching a source makes the project stale while leaving the build program's
# own declared inputs unchanged, which is exactly the state the replay serves.
#
# The boundary this leaves is deliberate and is documented in docs/07: an
# advisory appears whenever the build.mcpp stage is reached. A no-op build
# reaches nothing and prints nothing — including this. It is not asserted here,
# because it is a fact about the fast path rather than about this feature.
touch src/main.cpp
"$MCPP" build > second.log 2>&1

# First establish that this build really was a cache hit. Without this the next
# assertion could pass for the wrong reason — a re-run would also print the
# line, and the replay path would go untested.
grep -q "up to date (cached)" second.log \
  || { cat second.log; echo "FAIL: the second build re-ran the program; the replay path was not exercised"; exit 1; }

grep -q "advisory: no emulator found" second.log \
  || { cat second.log; echo "FAIL: the advisory was lost on a cache hit — it would appear once and never again"; exit 1; }

# ── 3. Two members, two advisories, each named ─────────────────────────────
#
# Attribution is the reason the package name is passed in rather than printed
# by the program: in a workspace the program cannot reliably spell which member
# it is.
cd "$TMP"
mkdir -p ws/a/src ws/b/src
cd ws

cat > mcpp.toml <<'EOF'
[workspace]
members = ["a", "b"]
EOF

for m in a b; do
  cat > $m/mcpp.toml <<EOF
[package]
name    = "$m"
version = "0.1.0"
EOF
  cat > $m/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
  cat > $m/build.mcpp <<EOF
import mcpp;
import std;
int main() { mcpp::warning("advisory from $m"); return 0; }
EOF
done

"$MCPP" build > ws.log 2>&1
grep -q "a: advisory from a" ws.log \
  || { cat ws.log; echo "FAIL: member a's advisory is missing or unattributed"; exit 1; }
grep -q "b: advisory from b" ws.log \
  || { cat ws.log; echo "FAIL: member b's advisory is missing or unattributed"; exit 1; }

echo "OK 139_build_program_advisory"
