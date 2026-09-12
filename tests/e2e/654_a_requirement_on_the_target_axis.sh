#!/usr/bin/env bash
# requires: elf gcc
# 654_a_requirement_on_the_target_axis.sh -- design 2026-09-12 (a UI
# framework on Android, iOS and Web), section 2.6, A6: `requires_abi` can sit
# directly under `[target.<sel>]` and under
# `[target.<sel>.feature-requires-abi]`, unioned into the package's
# requirement set only when the selector matches the resolved target.
#
# 647 already covers the UNCONDITIONAL forms (`[package] requires_abi`, and
# `[features.<f>] requires_abi`). This script exercises the TARGET axis:
#   1. a dependency's `[target.'cfg(linux)'] requires_abi = { threads = true }`
#      is refused on a Linux host while the root's abi table is absent,
#      naming the dependency and the selector `cfg(linux)`, as written;
#   2. with the root's `[target.'cfg(linux)'.abi] threads = true` the build
#      succeeds, and -pthread reaches the dependency's compile command;
#   3. the per-feature form (`[target.<sel>.feature-requires-abi]`) is
#      refused only when the feature that names it is active;
#   4. a selector that does not match the host (`cfg(windows)`) imposes
#      nothing, in both directions -- present or absent, the Linux build is
#      unaffected by a requirement scoped to a target it is not building.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/tgtdep/src"
cd "$TMP/tgtdep"
printf 'export module tgtdep;\nexport int tgt_value() { return 7; }\n' > src/tgtdep.cppm
write_dep() {   # $1 = extra [package]-level table text (requires_abi et al.)
    cat > "$TMP/tgtdep/mcpp.toml" <<TOML
[package]
name    = "tgtdep"
version = "0.1.0"

[targets.tgtdep]
kind = "lib"

[features]
mt = {}

$1
TOML
}

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > src/main.cpp <<'CPP'
import tgtdep;
int main() { return tgt_value() == 7 ? 0 : 1; }
CPP
write_app() {   # $1 = the dependency's inline table, $2 = trailing tables
    cat > "$TMP/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[dependencies]
tgtdep = $1

$2
TOML
}

newest_ninja() { ls -t $(find target -name build.ninja) | head -1; }
pthread_in_dep_command() {   # the compile command of tgtdep.cppm carries -pthread
    local cdb
    cdb=$(dirname "$(newest_ninja)")/compile_commands.json
    [ -f "$cdb" ] || cdb=$(ls -t $(find . -name compile_commands.json) | head -1)
    tr -d '\n' < "$cdb" | grep -o '{[^{}]*tgtdep\.cppm[^{}]*}' | grep -q -- '-pthread'
}

# ── 0. Control: a selector that does not match the host imposes nothing ────
write_dep $'[target.\'cfg(windows)\']\nrequires_abi = { threads = true }\n'
write_app '{ path = "../tgtdep" }' ''
"$MCPP" build > control.log 2>&1 \
    || fail "a windows-only target-axis requirement failed a host build" control.log
plain_dir=$(dirname "$(newest_ninja)")
if pthread_in_dep_command; then
    fail "control: -pthread is on the dependency's compile command with no root table" control.log
fi
echo "0. a non-matching selector imposes nothing OK"

# ── 1. A matching selector's requirement is refused without the root table ─
write_dep $'[target.\'cfg(linux)\']\nrequires_abi = { threads = true }\n'
write_app '{ path = "../tgtdep" }' ''
if "$MCPP" build > refuse-selector.log 2>&1; then
    fail "a target-axis threads requirement was built without the root's table" refuse-selector.log
fi
grep -qF "\`tgtdep\` requires the artefact's ABI to have threads ([target.'cfg(linux)'])" refuse-selector.log \
    || fail "the refusal does not name the dependency and the selector" refuse-selector.log
echo "1. refused, naming the dependency and cfg(linux) OK"

# ── 2. With the root's table, the build succeeds and threads reach the dep ──
write_app '{ path = "../tgtdep" }' $'[target.\'cfg(linux)\'.abi]\nthreads = true\n'
"$MCPP" build > threads.log 2>&1 || fail "the build with the root's table failed" threads.log
pthread_in_dep_command || fail "-pthread did not reach the dependency's compile command" threads.log
[ "$(dirname "$(newest_ninja)")" != "$plain_dir" ] \
    || fail "the build with threads reused the fingerprint directory of the plain build" threads.log
"$MCPP" run > run.log 2>&1 || fail "the program did not run" run.log
echo "2. the root's table satisfies the target-axis requirement OK"

# ── 3. The per-feature target-axis form is refused only when active ────────
rm -rf target
write_dep $'[target.\'cfg(linux)\'.feature-requires-abi]\nmt = { threads = true }\n'
write_app '{ path = "../tgtdep" }' ''
"$MCPP" build > feature-inactive.log 2>&1 \
    || fail "an inactive feature's target-axis requirement failed the build" feature-inactive.log
if pthread_in_dep_command; then
    fail "an inactive feature's requirement put -pthread on the compile command" feature-inactive.log
fi
echo "  3a. inactive feature imposes nothing OK"

write_app '{ path = "../tgtdep", features = ["mt"] }' ''
if "$MCPP" build > feature-active.log 2>&1; then
    fail "an active feature's target-axis requirement was built without the root's table" feature-active.log
fi
grep -qF "\`tgtdep\` requires the artefact's ABI to have threads ([target.'cfg(linux)'])" feature-active.log \
    || fail "the refusal does not name the selector for the active feature's requirement" feature-active.log
echo "  3b. active feature is refused, naming cfg(linux) OK"

write_app '{ path = "../tgtdep", features = ["mt"] }' $'[target.\'cfg(linux)\'.abi]\nthreads = true\n'
"$MCPP" build > feature-satisfied.log 2>&1 \
    || fail "the active feature's requirement was not satisfied by the root's table" feature-satisfied.log
pthread_in_dep_command || fail "-pthread did not reach the dependency's compile command" feature-satisfied.log
echo "3. the per-feature target-axis form OK"

echo "PASS: 654"
