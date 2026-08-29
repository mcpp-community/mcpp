#!/usr/bin/env bash
# requires: gcc
# 311_rule_build_dependencies.sh — a build rule may depend on another build
# rule, declared in its OWN `[build-dependencies]`.
#
# Before this, a rule was a leaf. Not by design: both live build-time channels
# (`tools`, `host-module`) are written by the CONSUMER on an edge, so a rule had
# no way to request anything on its own behalf. `[build-dependencies]` is the
# only declaration site a package owns, and it was read by nothing.
#
# The mechanism is ORDERING, not new machinery. build_program.cppm accumulates
# the module flags as it compiles the host modules in list order, so each entry
# already sees the BMIs of everything ahead of it; the caller topologically
# sorts the list and a rule can import a rule.
#
# Pinned here:
#   1. a rule imports its own `[build-dependencies]` host module and works;
#   2. the consumer may NOT import it — provisions cross one further edge only
#      on a `reexport = true` edge, and mcpp enforces that on every platform
#      because GCC cannot (its BMIs are reachable by name whatever the flags);
#   3. `reexport = true` makes it importable by the consumer;
#   4. a cycle is reported AS a cycle, naming the packages on it.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── the inner rule ──────────────────────────────────────────────────────────
mkdir -p globbing/src
cat > globbing/mcpp.toml <<'EOF'
[package]
name    = "globbing"
version = "0.1.0"

[targets.globbing]
kind = "lib"
EOF
cat > globbing/src/globbing.cppm <<'EOF'
export module globbing;
import std;
export namespace globbing {
inline int answer() { return 7; }
}
EOF

# ── the outer rule, which imports it ────────────────────────────────────────
mk_outer() {   # $1 = extra keys on the globbing edge
    mkdir -p tidyrule/src
    cat > tidyrule/mcpp.toml <<EOF
[package]
name    = "tidyrule"
version = "0.1.0"

[targets.tidyrule]
kind = "lib"

[build-dependencies]
globbing = { path = "../globbing", host-module = true$1 }
EOF
    cat > tidyrule/src/tidyrule.cppm <<'EOF'
export module tidyrule;
import std;
import mcpp;
import globbing;    // a rule importing a rule: impossible before this change
export namespace tidyrule {
inline void go() {
    mcpp::cxxflag(std::format("-DTIDY_ANSWER={}", globbing::answer()).c_str());
}
}
EOF
}
mk_outer ""

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
tidyrule = { path = "../tidyrule", host-module = true }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
#ifndef TIDY_ANSWER
#error "the outer rule never ran"
#endif
int main() { std::printf("ANSWER=%d\n", TIDY_ANSWER); }
EOF
cat > app/build.mcpp <<'EOF'
import mcpp;
import tidyrule;
int main() { tidyrule::go(); }
EOF

cd app
"$MCPP" build > b1.log 2>&1 || {
    cat b1.log; echo "FAIL: a rule with its own [build-dependencies] did not build"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
# The value travels through the inner rule, so it is evidence that the inner
# module was actually compiled and linked rather than merely named.
[[ "$out" == "ANSWER=7" ]] || {
    echo "FAIL: the inner rule's value did not reach the compile: $out"; exit 1; }

# The inner rule must be BUILD-time only: it has no business in the binary.
mapfile -t bins < <(find target -type f -name app -perm -u+x)
(( ${#bins[@]} == 1 )) || { echo "FAIL: expected one app binary, got ${#bins[@]}"; exit 1; }

# ── 2. the consumer may not import what it never declared ───────────────────
cd "$TMP"
cat > app/build.mcpp <<'EOF'
import mcpp;
import tidyrule;
import globbing;    // never declared by this package
int main() { tidyrule::go(); }
EOF
cd app && rm -rf target
if "$MCPP" build > b2.log 2>&1; then
    cat b2.log
    echo "FAIL: build.mcpp imported a module reachable only as another rule's prerequisite"
    exit 1
fi
grep -qF "globbing" b2.log || {
    cat b2.log; echo "FAIL: the diagnostic does not name the module"; exit 1; }
grep -qF "reexport" b2.log || {
    cat b2.log; echo "FAIL: the diagnostic does not say how to fix it"; exit 1; }

# ── 3. reexport = true makes it the consumer's to import ───────────────────
cd "$TMP"
mk_outer ", reexport = true"
cd app && rm -rf target
"$MCPP" build > b3.log 2>&1 || {
    cat b3.log; echo "FAIL: a re-exported host module was still refused"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=7" ]] || {
    echo "FAIL: the re-exported path broke the value: $out"; exit 1; }

# ── 4. a cycle is reported as a cycle ──────────────────────────────────────
cd "$TMP"
cat >> globbing/mcpp.toml <<'EOF'

[build-dependencies]
tidyrule = { path = "../tidyrule", host-module = true }
EOF
cd app && rm -rf target
if "$MCPP" build > b4.log 2>&1; then
    cat b4.log; echo "FAIL: a rule import cycle was accepted"; exit 1
fi
grep -qi "cycle" b4.log || {
    cat b4.log; echo "FAIL: the cycle was not reported as a cycle"; exit 1; }
# Naming the packages on the ring is the actionable half; a bare "cycle
# detected" leaves the reader to find it.
grep -qF "globbing" b4.log && grep -qF "tidyrule" b4.log || {
    cat b4.log; echo "FAIL: the cycle diagnostic does not name the packages on it"; exit 1; }

echo "OK"
