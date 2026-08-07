#!/usr/bin/env bash
# requires: linux
# 195_subos_env_reaches_program.sh — a subos's declared environment must reach
# the program mcpp launches (mcpp#352).
#
# A program needs three things: it links (bootstrap), it finds its libraries
# (RPATH), and it is told where its runtime data lives (env). mcpp supplied the
# first two and nothing for the third: xlings's graphics packages declare
# LIBGL_DRIVERS_PATH and friends into the subos, `xlings subos use` applied
# them, and `mcpp run` did not. That is why a GLFW binary could link cleanly
# and exit 255 with no output at all.
#
# The probe variable here is deliberately NOT a graphics one. mcpp does not
# know what any of these variables mean -- it carries whatever the subos
# declares -- and a test naming LIBGL_DRIVERS_PATH would quietly suggest
# otherwise.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A subos that declares one variable, in xlings's own schema. Built here rather
# than by mutating the developer's real subos: an earlier e2e wrote through a
# symlink and permanently broke a real toolchain, and MCPP_SUBOS_DIR exists so
# this test never has to go near one.
subos="$TMP/subos"
mkdir -p "$subos/usr/lib/dri"
cat > "$subos/.xlings.json" <<'EOF'
{ "workspace": {},
  "subos_info": { "schema_version": 1, "runtime": "glibc@2.39",
    "envs": [ { "binding": "probe@1", "decls": [
      { "var": "MCPP_E2E_PROBE", "op": "prepend",
        "value": "${subosdir}/usr/lib/dri" } ] } ] } }
EOF

cd "$TMP"
"$MCPP" new hello > /dev/null
cd hello
cat > src/main.cpp <<'EOF'
#include <cstdio>
#include <cstdlib>
int main() {
    const char* v = std::getenv("MCPP_E2E_PROBE");
    std::printf("PROBE=%s\n", v ? v : "(unset)");
    return 0;
}
EOF

# 1. `mcpp run` hands it to the program.
out=$(MCPP_SUBOS_DIR="$subos" "$MCPP" run 2>&1) || {
    echo "mcpp run failed:"; echo "$out"; exit 1; }
echo "$out" | grep -q "PROBE=$subos/usr/lib/dri" || {
    echo "the subos's declared environment did not reach the program:"
    echo "$out"
    exit 1
}

# 2. Without a subos saying anything, nothing is invented.
out2=$("$MCPP" run 2>&1) || { echo "plain mcpp run failed:"; echo "$out2"; exit 1; }
echo "$out2" | grep -q 'PROBE=(unset)' || {
    echo "a variable appeared with no subos declaring it:"
    echo "$out2"
    exit 1
}

# 3. A subos with no self-description degrades quietly and still runs. This is
#    the state of every subos created before xlings grew the block, so it must
#    not be an error.
bare="$TMP/bare"
mkdir -p "$bare"
echo '{ "workspace": {} }' > "$bare/.xlings.json"
out3=$(MCPP_SUBOS_DIR="$bare" "$MCPP" run 2>&1) || {
    echo "a subos without subos_info broke the run:"; echo "$out3"; exit 1; }
echo "$out3" | grep -q 'PROBE=(unset)' || {
    echo "unexpected output from a subos with no declarations:"; echo "$out3"; exit 1; }

echo "PASS: subos environment reaches the program, and nothing is invented"
