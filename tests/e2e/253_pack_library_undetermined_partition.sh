#!/usr/bin/env bash
# requires:
# (no capability: nothing here is toolchain-specific — the fixture uses a local
#  module rather than `import std`, and both assertions read mcpp's own output.)
#
# 253_pack_library_undetermined_partition.sh — when mcpp CANNOT TELL whether a
# published partition is an interface partition or an implementation one, it has
# to say so.
#
# `mcpp pack` publishes the module closure of the lib root as SOURCE. Whether a
# partition's source may travel depends on one keyword:
#
#   export module M:api;    interface partition       — publishing it is the point
#   module M:impl;          implementation partition  — publishing it is a leak
#
# The text scanner reads that keyword. Two other paths do not:
#
#   * `[scan_overrides."<glob>"]` names the modules a file provides and has
#     nowhere to say whether the declaration carries `export`;
#   * a P1689 scanner may omit `is-interface` (the key is optional).
#
# Both used to arrive as "interface" — the value that produces NO warning. So an
# implementation partition declared in `[scan_overrides]` was published in
# silence, which is the one failure mode the whole closure design exists to
# prevent.
#
# ⚠️ PINNED FROM BOTH SIDES, on purpose. Asserting only that the override case
# warns cannot distinguish "mcpp models three states" from "mcpp warns about
# every partition it publishes". So the same fixture is packed twice — once
# scanned, once overridden — and the two must produce DIFFERENT sentences:
#
#   scanned    → "secret.cppm is an implementation partition"   (it knows)
#   overridden → "cannot tell which kind"                       (it doesn't)
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
# The root interface reaches the partition, so the partition is published either
# way — this test is about WHAT MCPP SAYS while publishing it, not about whether
# it does (243 pins that).
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
import :secret;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/secret.cppm <<'EOF'
module mathkit:secret;
namespace mk { int secret_bias() { return 40; } }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk {
int secret_bias();
int answer() { return secret_bias() + 2; }
}
EOF

manifest() {   # $1 = extra manifest body (the override, or nothing)
    # Absolute: this is called from inside mathkit/ the second time, and a
    # relative path there writes a manifest nobody reads (or nothing at all).
    cat > "$TMP/mathkit/mcpp.toml" <<EOF
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
$1
EOF
}

# ── side 1: scanned. mcpp read the keyword, so it knows. ────────────────
manifest ''
cd mathkit
rm -rf target
"$MCPP" pack mathkit > scanned.log 2>&1 || { cat scanned.log; echo "pack failed"; exit 1; }
grep -q 'secret.cppm is an implementation partition' scanned.log || {
    cat scanned.log
    echo "FAIL: the scanned case lost its known-implementation-partition warning"
    exit 1; }
grep -q 'cannot tell which kind' scanned.log && {
    cat scanned.log
    echo "FAIL: mcpp read \`module mathkit:secret;\` and still claims it cannot tell."
    echo "      Then the undetermined state is not a state, it is every partition,"
    echo "      and the warning carries no information."
    exit 1; }

# ── side 2: overridden. Nobody told mcpp whether it is exported. ────────
#
# The override declares exactly what the scanner would have found, EXCEPT the
# one thing the schema cannot express — so the graph is identical apart from the
# unknown, and any difference in output is attributable to it alone.
manifest '
[scan_overrides."src/secret.cppm"]
provides = ["mathkit:secret"]'
rm -rf target
"$MCPP" pack mathkit > overridden.log 2>&1 || { cat overridden.log; echo "override pack failed"; exit 1; }
grep -q 'cannot tell which kind' overridden.log || {
    cat overridden.log
    echo "FAIL: a partition declared in [scan_overrides] was published without a word."
    echo "      That is the silent half of the asymmetry: too FEW published sources"
    echo "      fails the consumer's compile, too MANY ships private source and"
    echo "      nothing fails. Unknown must warn."
    exit 1; }
grep -q 'secret.cppm is an implementation partition' overridden.log && {
    cat overridden.log
    echo "FAIL: mcpp asserted it IS an implementation partition. Nothing told it so —"
    echo "      the override cannot say, and stating it anyway is a guess wearing"
    echo "      a diagnostic's clothes."
    exit 1; }

# It is still published: the consumer cannot build the root's BMI without it.
pkg="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
[[ -e "$pkg/interface/secret.cppm" ]] || {
    echo "FAIL: the warning fired but the source was not published — the consumer"
    echo "      would fail to compile the interface it was shipped."
    exit 1; }
PKG_HOST="$(host_path "$TMP/mathkit/$pkg")"

# ── and the package a warned-about pack produces still works ────────────
cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = { path = "$PKG_HOST" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
( cd app && "$MCPP" run > run.log 2>&1 ) || { cat app/run.log; echo "consumer failed"; exit 1; }
grep -q 'ok=42' app/run.log || { cat app/run.log; echo "wrong answer"; exit 1; }

echo "PASS: an undetermined partition is published loudly, and it says so differently"
