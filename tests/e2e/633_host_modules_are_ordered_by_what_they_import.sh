#!/usr/bin/env bash
# requires: gcc
# 633_host_modules_are_ordered_by_what_they_import.sh — the units a host-module
# package contributes are compiled in the order their imports require, not in
# the order their paths sort.
#
# THE DEFECT. `build_program.cppm` accumulates module flags as it compiles a
# package's host modules, so each unit sees only the BMIs of those ahead of it.
# The list was the lib root followed by every other declared interface unit in
# PATH order -- a `std::set` of paths. `rules/spirv.cppm` sorts before
# `src/surface.cppm`, so a member importing a unit its package shares was
# compiled first and failed with:
#
#   failed to read compiled module: No such file or directory
#   note: imports must be built before being imported
#
# Reproduced in both directions before the fix: renaming the shared unit so its
# path sorted first made the SAME package build, which is what says the cause is
# the sort and nothing else.
#
# WHAT IT COST. `mcpp:plugins` read that failure as "a second unit is not
# compiled as a host module at all" and folded everything its members shared
# into the lib root, taking it from about twenty lines to about seven hundred.
# The cause was a sort.
#
# LEG 1 IS THE REVERSED CHAIN. Three units whose import order is the exact
# reverse of their path order:
#
#   rules/thing.cppm   probe.rules.thing  imports probe.mid
#   src/mm_mid.cppm    probe.mid          imports probe.base
#   src/zz_base.cppm   probe.base
#
# Under a path sort every one of them precedes what it imports. Under an import
# sort every one follows it. Nothing but the ordering decides whether this
# builds.
#
# LEG 2 IS THE UNCONSTRAINED CASE, and it is why the sort keeps path order as
# its tie-break: a package with no internal imports must be ordered exactly as
# it was, so a package that works today cannot be broken by this change.
#
# THE CRITERION IS A VALUE THE MODULES COMPUTE TOGETHER, surfaced through
# `mcpp::warning` because that is the one build-program channel `mcpp build`
# prints on success -- its stdout is shown only when the program FAILS, so
# grepping for anything else would be vacuous.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ── leg 1: the reversed chain ───────────────────────────────────────────────
mkdir -p "$TMP/probe/src" "$TMP/probe/rules" "$TMP/app/src"

cat > "$TMP/probe/mcpp.toml" <<'EOF'
[package]
name      = "probe"
namespace = "ex"
version   = "0.1.0"

[language]
standard = "c++23"
modules  = true

[build]
sources = ["src/probe.cppm", "src/mm_mid.cppm", "src/zz_base.cppm"]

[features]
default = []
thing = { sources = ["rules/thing.cppm"] }
EOF

cat > "$TMP/probe/src/probe.cppm" <<'EOF'
export module probe;
export namespace probe { inline int root() { return 1000; } }
EOF

cat > "$TMP/probe/src/zz_base.cppm" <<'EOF'
export module probe.base;
export namespace probe::base { inline int value() { return 40; } }
EOF

cat > "$TMP/probe/src/mm_mid.cppm" <<'EOF'
export module probe.mid;
import probe.base;
export namespace probe::mid { inline int value() { return probe::base::value() + 1; } }
EOF

cat > "$TMP/probe/rules/thing.cppm" <<'EOF'
export module probe.rules.thing;
import probe;
import probe.mid;
export namespace probe::rules::thing {
inline int value() { return probe::root() + probe::mid::value() + 1; }
}
EOF

cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[language]
standard = "c++23"
modules  = true

[build-dependencies.ex]
probe = { path = "../probe", features = ["thing"], host-module = true }
EOF

cat > "$TMP/app/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF

cat > "$TMP/app/build.mcpp" <<'EOF'
#include <string>
import mcpp;
import probe.rules.thing;
int main() {
    const std::string m = "CHAIN=" + std::to_string(probe::rules::thing::value());
    mcpp::warning(m.c_str());
}
EOF

cd "$TMP/app"
"$MCPP" build > b1.log 2>&1 || {
    cat b1.log
    echo "FAIL: a package whose units import each other was compiled in path order"
    exit 1
}
grep -q 'CHAIN=1042' b1.log || {
    cat b1.log
    echo "FAIL: the chain did not compute 1000 + 41 + 1; the units did not all link"
    exit 1
}

# ── leg 2: nothing constrains the order, so path order must survive ─────────
#
# Not decoration. A topological sort that ignored the original order would be
# free to emit any valid order, and a package that works today would be
# reordered for no reason -- silent until something depended on it. This asserts
# the sort is stable where it has no constraint to satisfy.
rm -rf "$TMP/probe/src/mm_mid.cppm" "$TMP/probe/src/zz_base.cppm" b1.log
cat > "$TMP/probe/mcpp.toml" <<'EOF'
[package]
name      = "probe"
namespace = "ex"
version   = "0.1.0"

[language]
standard = "c++23"
modules  = true

[build]
sources = ["src/probe.cppm", "src/aa_one.cppm", "src/bb_two.cppm"]

[features]
default = []
thing = { sources = ["rules/thing.cppm"] }
EOF
cat > "$TMP/probe/src/aa_one.cppm" <<'EOF'
export module probe.one;
export namespace probe::one { inline int value() { return 7; } }
EOF
cat > "$TMP/probe/src/bb_two.cppm" <<'EOF'
export module probe.two;
export namespace probe::two { inline int value() { return 35; } }
EOF
cat > "$TMP/probe/rules/thing.cppm" <<'EOF'
export module probe.rules.thing;
import probe;
import probe.one;
import probe.two;
export namespace probe::rules::thing {
inline int value() { return probe::root() + probe::one::value() + probe::two::value(); }
}
EOF

rm -rf target
"$MCPP" build > b2.log 2>&1 || {
    cat b2.log
    echo "FAIL: a package with no internal imports stopped building"
    exit 1
}
grep -q 'CHAIN=1042' b2.log || {
    cat b2.log; echo "FAIL: the unconstrained case did not compute 1000 + 7 + 35"; exit 1; }

echo "PASS: 633 host modules are ordered by what they import"
