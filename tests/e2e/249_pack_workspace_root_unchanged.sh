#!/usr/bin/env bash
# requires:
# (no capability: a library package is claimed to work on every target, so this
#  test has to RUN on every platform. `# requires: gcc` would have skipped it on
#  macOS and Windows — Apple Clang is not the gcc capability — leaving the claim
#  unverified while the suite stayed green.)
# 249_pack_workspace_root_unchanged.sh — `mcpp pack` in a workspace root still
# packs the member's program.
#
# Routing `mcpp pack` through `[targets.<n>].kind` means something has to read
# the manifest before the build does. A workspace root has no targets of its
# own — a virtual one has no `[package]` either — so the first version of that
# routing read the root's empty target list and concluded there was nothing to
# pack, turning a working command into an error.
#
# Caught by running the previous release against examples/04-workspace and
# comparing. This test is that comparison, made permanent.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p ws/apps/hello/src ws/libs/mathcore/src

cat > ws/mcpp.toml <<'EOF'
[workspace]
members = ["libs/mathcore", "apps/hello"]
EOF

cat > ws/libs/mathcore/mcpp.toml <<'EOF'
[package]
name    = "mathcore"
version = "0.1.0"
EOF
cat > ws/libs/mathcore/src/mathcore.cppm <<'EOF'
export module mathcore;
export int core_answer() { return 42; }
EOF

cat > ws/apps/hello/mcpp.toml <<'EOF'
[package]
name    = "hello"
version = "0.1.0"
[dependencies]
mathcore = { path = "../../libs/mathcore" }
EOF
cat > ws/apps/hello/src/main.cpp <<'EOF'
#include <cstdio>
import mathcore;
int main() { std::printf("ok=%d\n", core_answer()); return 0; }
EOF

cd ws
"$MCPP" pack --mode system > pack.log 2>&1 || {
    cat pack.log
    echo "FAIL: packing from a virtual workspace root stopped working"
    exit 1
}
grep -q 'Packed' pack.log || { cat pack.log; echo "no archive reported"; exit 1; }
[[ -n "$(find . -name 'hello-0.1.0-*.tar.gz' | head -1)" ]] || {
    cat pack.log; echo "the member's archive was not produced"; find . -name '*.tar.gz'; exit 1; }

echo "PASS: a workspace root still packs its member's program"
