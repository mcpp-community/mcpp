#!/usr/bin/env bash
# requires: gcc
# 186_build_mcpp_protocol_and_bound.sh — the build.mcpp CONTRACT hardening:
# wire-protocol version, the cache's semantic epoch, and the run bound.
#
# What each part protects against:
#   * protocol   — a build.mcpp written for a newer mcpp used to have its
#                  unknown directives WARNED about and dropped, producing a
#                  silently different build. It must now refuse.
#   * legacy     — a hand-written printf program announces nothing; its surface
#                  is frozen, so its unknown keys stay a warning (a typo), not
#                  an error. This asymmetry is the compatibility contract and
#                  has to be pinned by a test or it will be "simplified" away.
#   * epoch      — a cache entry written under a different directive
#                  interpretation must not be replayed under this one.
#   * run bound  — a build program that hangs used to hang the whole build with
#                  no diagnostic at all.
#
# See .agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md §4.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cd app

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF

cat > src/main.cpp <<'EOF'
#ifndef FROM_BUILD_MCPP
#error "define missing"
#endif
int main() { return 0; }
EOF

fresh() { rm -rf target; }

# ── 1. import mcpp; announces a protocol, so an unknown directive is fatal ──
cat > build.mcpp <<'EOF'
#include <cstdio>
import mcpp;
int main() {
    mcpp::cxxflag("-DFROM_BUILD_MCPP=1");
    std::printf("mcpp:no-such-directive=1\n");
}
EOF
fresh
if "$MCPP" build > b1.log 2>&1; then
    cat b1.log; echo "FAIL: unknown directive from an announcing program was accepted"; exit 1
fi
grep -q "no-such-directive" b1.log || {
    cat b1.log; echo "FAIL: error does not name the offending directive"; exit 1; }
# The message must explain WHY it is fatal here but not for a printf program,
# otherwise the asymmetry reads as a bug.
grep -qi "protocol" b1.log || {
    cat b1.log; echo "FAIL: error does not mention the protocol"; exit 1; }

# ── 2. A hand-written printf program keeps warn-and-ignore ──────────────────
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    std::printf("mcpp:cxxflag=-DFROM_BUILD_MCPP=1\n");
    std::printf("mcpp:no-such-directive=1\n");
}
EOF
fresh
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: legacy printf program was rejected"; exit 1; }
grep -q "ignoring unknown directive" b2.log || {
    cat b2.log; echo "FAIL: legacy program lost its warn-and-ignore behaviour"; exit 1; }

# ── 3. A program claiming a newer protocol is refused with an upgrade hint ──
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    std::printf("mcpp:protocol=999\n");
    std::printf("mcpp:cxxflag=-DFROM_BUILD_MCPP=1\n");
}
EOF
fresh
if "$MCPP" build > b3.log 2>&1; then
    cat b3.log; echo "FAIL: a newer-protocol program was accepted"; exit 1
fi
grep -q "999" b3.log || { cat b3.log; echo "FAIL: error does not name the claimed protocol"; exit 1; }
grep -qi "upgrade" b3.log || { cat b3.log; echo "FAIL: error gives no actionable next step"; exit 1; }

# ── 4. Run bound: a hanging program is killed and the error says so ─────────
cat > build.mcpp <<'EOF'
#include <cstdio>
#include <unistd.h>
int main() {
    std::printf("mcpp:cxxflag=-DFROM_BUILD_MCPP=1\n");
    std::fflush(stdout);
    for (;;) ::sleep(60);
}
EOF
fresh
start=$(date +%s)
if MCPP_BUILD_PROGRAM_TIMEOUT=3 "$MCPP" build > b4.log 2>&1; then
    cat b4.log; echo "FAIL: a hanging build.mcpp did not fail the build"; exit 1
fi
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -lt 60 ] || { echo "FAIL: the bound did not fire (took ${elapsed}s)"; exit 1; }
grep -q "time limit" b4.log || { cat b4.log; echo "FAIL: no timeout diagnostic"; exit 1; }
# It must name the package: in a workspace or a dependency graph, "build.mcpp
# hung" is useless without knowing whose.
grep -q "'app'" b4.log || { cat b4.log; echo "FAIL: timeout error does not name the package"; exit 1; }
grep -q "MCPP_BUILD_PROGRAM_TIMEOUT" b4.log || {
    cat b4.log; echo "FAIL: timeout error does not say how to change the bound"; exit 1; }

# ── 5. Cache: the entry carries an epoch, and a foreign one invalidates it ──
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() { std::printf("mcpp:cxxflag=-DFROM_BUILD_MCPP=1\n"); }
EOF
fresh
"$MCPP" build > b5.log 2>&1 || { cat b5.log; echo "FAIL: build failed"; exit 1; }
CACHE=target/.build-mcpp/build.mcpp.cache
[ -f "$CACHE" ] || { echo "FAIL: no build.mcpp cache written"; exit 1; }
grep -q '^epoch ' "$CACHE" || { cat "$CACHE"; echo "FAIL: cache carries no epoch"; exit 1; }
cp "$CACHE" "$TMP/good.cache"

# Touching a source defeats the whole-project fast path so prepare (and with it
# the build.mcpp cache) actually runs.
touch src/main.cpp
"$MCPP" build > b6.log 2>&1 || { cat b6.log; echo "FAIL: build failed"; exit 1; }
grep -q "up to date (cached)" b6.log || {
    cat b6.log; echo "FAIL: an unchanged build.mcpp was re-run"; exit 1; }

sed 's/^epoch .*/epoch 987654/' "$TMP/good.cache" > "$CACHE"
touch src/main.cpp
"$MCPP" build > b7.log 2>&1 || { cat b7.log; echo "FAIL: build failed"; exit 1; }
grep -q "build.mcpp running" b7.log || {
    cat b7.log; echo "FAIL: a foreign cache epoch did not force a re-run"; exit 1; }

# A `d` record this mcpp cannot interpret (a cache written by a NEWER mcpp)
# must invalidate the entry too — replaying the rest would apply a strict
# subset of what the program asked for.
cp "$TMP/good.cache" "$CACHE"
echo "d some-future-tag /x" >> "$CACHE"
touch src/main.cpp
"$MCPP" build > b8.log 2>&1 || { cat b8.log; echo "FAIL: build failed"; exit 1; }
grep -q "build.mcpp running" b8.log || {
    cat b8.log; echo "FAIL: an unknown cache record did not force a re-run"; exit 1; }

echo "OK"
