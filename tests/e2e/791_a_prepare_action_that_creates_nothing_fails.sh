#!/usr/bin/env bash
# requires: gcc
# 791_a_prepare_action_that_creates_nothing_fails.sh — design §5.4 P, SPEC-007
# R3.3: "when the command succeeds and the directory does not exist, the
# engine writes no stamp and fails the edge, naming the directory." A
# `prepare` action promises a directory the build reads BY DIRECTORY
# (`include_dir`/`link_search`/`runtime_search_dir`); a command that exits 0
# without populating it is indistinguishable from one that silently did
# nothing, so this cannot be a warning the way an incomplete environment is
# (R1.2) -- nothing downstream can read a directory that was never created.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
mkdir -p "$TMP/app/src"
cd "$TMP/app"

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF
printf '#include <cstdio>\nint main(){ std::printf("ok\\n"); }\n' > src/main.cpp

cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    const std::string prefix = std::string(mcpp::out_dir()) + "/never-created";
    mcpp::action a;
    a.id   = "empty:prepare";
    a.role = mcpp::roles::prepare;
    // The stamp lives BESIDE `prefix`, not inside it: ninja creates a
    // declared output's own parent directory before running any edge, so a
    // stamp nested inside `prefix` would make `prefix` exist by the time the
    // engine checks for it regardless of what the command did, and this
    // test would defend nothing.
    a.arg("true")
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();
    return 0;
}
EOF

MCPP="${MCPP:-mcpp}"
if "$MCPP" build > b.log 2>&1; then
    cat b.log
    echo "FAIL: a prepare action whose command created nothing let the build succeed"
    exit 1
fi

grep -qF "never-created" b.log \
    || { cat b.log; echo "FAIL: the failure does not name the declared directory"; exit 1; }
grep -qi "does not exist" b.log \
    || { cat b.log; echo "FAIL: the failure does not say the directory does not exist"; exit 1; }

# No half-satisfied edge: the stamp itself must not exist either, or a later
# build would read a check that "passed" without its post-condition holding.
if find target -name 'never-created.stamp' | grep -q .; then
    echo "FAIL: a stamp was written despite the missing directory"; exit 1
fi

echo "PASS: 791_a_prepare_action_that_creates_nothing_fails"
