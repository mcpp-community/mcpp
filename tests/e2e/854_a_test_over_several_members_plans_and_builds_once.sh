#!/usr/bin/env bash
# requires: unix-shell
# 854 -- `mcpp test` over several members plans once per configuration group
# and builds what the members share once.
#
# `test --workspace` used to plan each member alone (`workspace_fanout_members`
# and `run_tests` once per member), so a member two members use had its build
# program run once per member that reached it, and its features were those of
# one member's closure, not the union across the selection. `build` and `emit`
# already planned a selection once per configuration group and gave each
# member's tests to the plan in `member_targets`; `test` now does the same
# (member selection design 2026-09-30, S4).
#
# Criteria:
#   F. `a` and `b` share `core`, whose build program appends a line to a file
#      under its OUT_DIR on each run; `a` asks for `core`'s feature `extra`.
#      After `mcpp test --workspace` the file has one line (it had two), the
#      plan states its members once, and both members' tests, each named
#      `main`, run and pass.
#   G. After `mcpp build --workspace`, `mcpp test --workspace` adds no compile
#      edge for `core`'s sources and runs `core`'s program no more. The
#      fixture has no dev-dependency, so the test build does not change
#      `core`'s features.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
cd "$TMP"

mkdir -p core/src a/src a/tests b/src b/tests
cat > mcpp.toml <<'EOF'
[workspace]
members = ["core", "a", "b"]
EOF
cat > core/mcpp.toml <<'EOF'
[package]
name    = "core"
version = "0.1.0"

[features]
extra = []

[targets.core]
kind = "lib"
EOF
cat > core/src/core.cppm <<'EOF'
export module sel854_core;
export int core_v() { return 1; }
export int core_extra() {
#ifdef MCPP_FEATURE_EXTRA
    return 1;
#else
    return 0;
#endif
}
EOF
cat > core/build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    std::ofstream(std::string(mcpp::out_dir()) + "/runs.log", std::ios::app) << "ran\n";
    return 0;
}
EOF
cat > a/mcpp.toml <<'EOF'
[package]
name    = "a"
version = "0.1.0"

[dependencies]
core = { path = "../core", features = ["extra"] }

[targets.a]
kind = "lib"
EOF
cat > b/mcpp.toml <<'EOF'
[package]
name    = "b"
version = "0.1.0"

[dependencies]
core = { path = "../core" }

[targets.b]
kind = "lib"
EOF
for m in a b; do
    printf 'export module sel854_%s;\nimport sel854_core;\nexport int %s_v() { return core_v(); }\n' $m $m > $m/src/$m.cppm
    # Both members name their test `main`: discovery is scoped to each member.
    printf 'import sel854_%s;\nint main() { return %s_v() == 1 ? 0 : 1; }\n' $m $m > $m/tests/main.cpp
done

runs() { find . -name runs.log -exec cat {} + | wc -l | tr -d ' '; }
core_edges() { awk -F'\t' '$4 ~ /(^|\/)obj\/core\/src\/core\.m\.o$/' "$1/.ninja_log" | wc -l | tr -d ' '; }
build_dir() { find target -name build.ninja -not -path '*/.build-mcpp/*' -exec dirname {} \; | head -1; }
reset() { rm -rf target core/target a/target b/target; }

# ── F ──────────────────────────────────────────────────────────────────────
reset
"$MCPP" test --workspace > f.log 2>&1 || fail "F: test --workspace failed" f.log
[ "$(runs)" = 1 ] || fail "F: core's build program ran $(runs) times; one plan runs it once" f.log
[ "$(grep -c 'Workspace building' f.log)" = 1 ] || fail "F: the members were planned more than once" f.log
grep -q "Workspace building 2 members: a, b" f.log || fail "F: the plan does not state its two members" f.log
grep -q "member 'a' (2/3) ok — 1 passed" f.log && grep -q "member 'b' (3/3) ok — 1 passed" f.log \
    || fail "F: a and b did not both run their test" f.log
grep -q "workspace result ok. 3 member(s); 2 passed" f.log || fail "F: the workspace result is not that of the two tests" f.log
[ "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | wc -l | tr -d ' ')" = 1 ] || fail "F: the members were built in more than one directory" f.log
[ "$(core_edges "$(build_dir)")" = 1 ] || fail "F: core was compiled more than once" "$(build_dir)/.ninja_log"
echo "ok: F, the members' shared build program ran once, in one plan, and each member's tests ran"

# ── G ──────────────────────────────────────────────────────────────────────
reset
"$MCPP" build --workspace > g1.log 2>&1 || fail "G: build --workspace failed" g1.log
dir=$(build_dir)
before=$(core_edges "$dir")
[ "$before" = 1 ] && [ "$(runs)" = 1 ] || fail "G: the build did not compile and run core once" g1.log
"$MCPP" test --workspace > g2.log 2>&1 || fail "G: test --workspace failed" g2.log
[ "$(build_dir)" = "$dir" ] || fail "G: the test build used another directory" g2.log
[ "$(core_edges "$dir")" = "$before" ] || fail "G: test --workspace compiled core again" "$dir/.ninja_log"
[ "$(runs)" = 1 ] || fail "G: test --workspace ran core's build program again" g2.log
grep -q "workspace result ok. 3 member(s); 2 passed" g2.log || fail "G: the tests did not run" g2.log
echo "ok: G, a test after a build compiles nothing of core and runs its program no more"

echo "PASS: 854_a_test_over_several_members_plans_and_builds_once"
