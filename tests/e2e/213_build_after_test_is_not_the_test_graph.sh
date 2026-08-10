#!/usr/bin/env bash
# requires: gcc
# 213_build_after_test_is_not_the_test_graph.sh — mcpp#407.
#
# `mcpp build`, `mcpp test` and `mcpp build --configure-only` all write
# `target/<triple>/<fp>/build.ninja`, and they land in the same directory
# because the fingerprint covers neither dev-dependencies nor test targets.
# A test-mode plan's `default` line names the TEST binaries and does not
# contain the package's own target at all.
#
# The fast path used to check build.ninja's mtime against the SOURCES and
# nothing else, so:
#
#   mcpp build   → default bin/<pkg>
#   mcpp test    → default bin/<test>      (same file, rewritten)
#   mcpp build   → Finished in 0.00s       (asked ninja for bin/<test>)
#
# reported success for a build that never linked the target. And because
# `tests/` is not in the source sweep, breaking a test file made a plain
# `mcpp build` FAIL with src/ untouched.
#
# DO NOT DELETE ARTIFACTS ANYWHERE IN THIS FILE. A missing output makes ninja
# fail in the shape of a stale graph, the fast path falls back to a full
# prepare, and an unfixed binary goes green.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/proj/src" "$TMP/proj/tests"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name = "tst"
version = "0.1.0"
EOF
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > tests/smoke.cpp <<'EOF'
int main() { return 0; }
EOF

find_ninja() { find "$TMP/proj/target" -name build.ninja | head -1; }
default_line() { grep -E '^default ' "$1" | head -1; }

# ── 1. plain build ──────────────────────────────────────────────────────────
"$MCPP" build > b1.log 2>&1 || { cat b1.log; exit 1; }
N="$(find_ninja)"
[[ -n "$N" ]] || { echo "FAIL: no build.ninja after the first build"; exit 1; }

BIN="$(dirname "$(dirname "$N")")"   # unused guard; the real path is globbed below
target_bin() { ls target/*/*/bin/tst 2>/dev/null | head -1; }

[[ -n "$(target_bin)" ]] || {
    echo "FAIL: plain build produced no bin/tst"
    cat b1.log
    exit 1
}

# The graph must say what it is. Without this line there is nothing for the
# fast path to check, and the rest of this test would pass for the wrong
# reason on a binary that simply always does a full prepare.
grep -q '^# mcpp:graph=normal' "$N" || {
    echo "FAIL: a plain build's build.ninja does not declare graph=normal"
    head -3 "$N"
    exit 1
}

# ── 2. mcpp test rewrites the same file with the test graph ─────────────────
"$MCPP" test > t1.log 2>&1 || { cat t1.log; exit 1; }
N2="$(find_ninja)"
[[ "$N2" == "$N" ]] || {
    echo "NOTE: test used a different build.ninja ($N2); #407 needs the shared one"
}
grep -q '^# mcpp:graph=test' "$N2" || {
    echo "FAIL: mcpp test's build.ninja does not declare graph=test"
    head -3 "$N2"
    exit 1
}

# ── 3. THE assertion: a plain build after a test must not replay that graph ─
#
# NOTHING IS TOUCHED HERE. Not the sources, not the outputs. An earlier draft
# ran `touch src/main.cpp` to make the relink observable — which invalidates
# the fast path by mtime and makes the test pass on an unfixed binary. The
# defect only exists while the fast path is ELIGIBLE, so the test has to leave
# every input alone and read the graph instead.
"$MCPP" build > b2.log 2>&1 || {
    echo "FAIL: plain build after mcpp test failed"
    cat b2.log
    exit 1
}
N3="$(find_ninja)"
grep -q '^# mcpp:graph=normal' "$N3" || {
    echo "FAIL: plain build replayed the test graph (build.ninja still says test)"
    echo "      this is mcpp#407: the fast path never checked what graph it had"
    head -3 "$N3"
    cat b2.log
    exit 1
}
case "$(default_line "$N3")" in
    *bin/tst*) ;;
    *)
        echo "FAIL: default line still names the test graph: $(default_line "$N3")"
        exit 1
        ;;
esac
[[ -n "$(target_bin)" ]] || {
    echo "FAIL: bin/tst is gone after a plain build"
    exit 1
}

# ── 4. a broken TEST file must not fail a plain build ───────────────────────
# src/ is untouched here. Before the fix this failed with a compile error in
# tests/smoke.cpp, from `mcpp build`.
cat > tests/smoke.cpp <<'EOF'
this is not valid C++ at all
EOF
"$MCPP" build > b3.log 2>&1 || {
    echo "FAIL: a broken tests/*.cpp failed a plain mcpp build"
    cat b3.log
    exit 1
}

echo "PASS: a plain build never replays the test graph, and tests/ cannot break it"
