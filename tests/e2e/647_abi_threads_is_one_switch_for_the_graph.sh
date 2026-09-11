#!/usr/bin/env bash
# requires: elf
# 647_abi_threads_is_one_switch_for_the_graph.sh -- `[target.<selector>.abi]
# threads` and `requires_abi` (design 2026-09-12, section 5.2).
#
# The threads ABI is a property of the artefact: every translation unit, the
# standard library module and the link must agree, so the root decides it and a
# dependency states that it needs it. Asserted on an ELF host, where the switch
# renders as -pthread:
#   1. a dependency that requires threads is refused while the root does not
#      state them, naming the dependency and what required them (the package, or
#      one of its features);
#   2. with the root's `[target.'cfg(os = "linux")'.abi] threads = true` the
#      build succeeds, -pthread reaches the dependency's compile command, and the
#      build lands in a different fingerprint directory than one without it;
#   3. a dependency that writes the table itself is reported and changes nothing;
#   4. a table scoped to another target changes nothing in a host build.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

ABI_TABLE=$(cat <<'TOML'
[target.'cfg(os = "linux")'.abi]
threads = true
TOML
)

mkdir -p "$TMP/mtdep/src"
cd "$TMP/mtdep"
printf 'export module mtdep;\nexport int mt_value() { return 5; }\n' > src/mtdep.cppm
write_dep() {   # $1 = extra [package] lines, $2 = trailing tables
    cat > mcpp.toml <<TOML
[package]
name    = "mtdep"
version = "0.1.0"
$1

[targets.mtdep]
kind = "lib"

[features]
mt = { requires_abi = { threads = true } }

$2
TOML
}

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > src/main.cpp <<'CPP'
import mtdep;
int main() { return mt_value() == 5 ? 0 : 1; }
CPP
write_app() {   # $1 = the dependency's inline table, $2 = trailing tables
    cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mtdep = $1

$2
TOML
}

newest_ninja() { ls -t $(find target -name build.ninja) | head -1; }
pthread_in_dep_command() {   # the compile command of mtdep.cppm carries -pthread
    local cdb
    cdb=$(dirname "$(newest_ninja)")/compile_commands.json
    [ -f "$cdb" ] || cdb=$(ls -t $(find . -name compile_commands.json) | head -1)
    tr -d '\n' < "$cdb" | grep -o '{[^{}]*mtdep\.cppm[^{}]*}' | grep -q -- '-pthread'
}

# ── Control: no requirement, no table ─────────────────────────────────────
write_dep '' ''
write_app '{ path = "../mtdep" }' ''
"$MCPP" build > plain.log 2>&1 || fail "the plain build failed" plain.log
plain_dir=$(dirname "$(newest_ninja)")
if pthread_in_dep_command; then
    fail "control: -pthread is already on the dependency's compile command without the switch" plain.log
fi

# A table scoped to another target changes nothing in a host build.
SCOPED_TABLE=$(cat <<'TOML'
[target.'cfg(os = "emscripten")'.abi]
threads = true
TOML
)
write_app '{ path = "../mtdep" }' "$SCOPED_TABLE"
"$MCPP" build > scoped.log 2>&1 || fail "a table scoped to emscripten failed a host build" scoped.log
[ "$(dirname "$(newest_ninja)")" = "$plain_dir" ] \
    || fail "a table scoped to emscripten moved the host build to another fingerprint directory" scoped.log
if pthread_in_dep_command; then
    fail "a table scoped to emscripten put -pthread on a host build" scoped.log
fi
echo "scoped table inert on the host OK"

# ── 1. Refused while the root does not state threads ──────────────────────
write_dep 'requires_abi = { threads = true }' ''
if "$MCPP" build > refuse-package.log 2>&1; then
    fail "a dependency requiring threads was built without them" refuse-package.log
fi
grep -qF "\`mtdep\` requires the artefact's ABI to have threads (the package)" refuse-package.log \
    || fail "the refusal does not name the package requirement" refuse-package.log
grep -qF 'threads = true' refuse-package.log \
    || fail "the refusal does not show the table that states it" refuse-package.log

write_dep '' ''
write_app '{ path = "../mtdep", features = ["mt"] }' ''
if "$MCPP" build > refuse-feature.log 2>&1; then
    fail "a feature requiring threads was built without them" refuse-feature.log
fi
grep -qF "\`mtdep\` requires the artefact's ABI to have threads (feature \`mt\`)" refuse-feature.log \
    || fail "the refusal does not name the feature" refuse-feature.log
echo "refusals OK"

# ── 2. The root states it ─────────────────────────────────────────────────
write_app '{ path = "../mtdep", features = ["mt"] }' "$ABI_TABLE"
"$MCPP" build > threads.log 2>&1 || fail "the build with threads failed" threads.log
pthread_in_dep_command || fail "-pthread did not reach the dependency's compile command" threads.log
[ "$(dirname "$(newest_ninja)")" != "$plain_dir" ] \
    || fail "the build with threads reused the fingerprint directory of the build without" threads.log
"$MCPP" run > run.log 2>&1 || fail "the program did not run" run.log
echo "switch OK"

# ── 3. A dependency's own table is reported and changes nothing ───────────
rm -rf target
write_dep '' "$ABI_TABLE"
write_app '{ path = "../mtdep" }' ''
"$MCPP" build > dep-table.log 2>&1 || fail "a dependency's abi table failed the build" dep-table.log
grep -qF "\`mtdep\` declares [target.<selector>.abi], which only the root manifest decides" dep-table.log \
    || fail "a dependency's abi table was not reported" dep-table.log
if pthread_in_dep_command; then
    fail "a dependency's abi table changed the dependency's compile command" dep-table.log
fi
echo "dependency table OK"
