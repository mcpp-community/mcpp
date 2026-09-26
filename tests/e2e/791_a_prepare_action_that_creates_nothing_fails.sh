#!/usr/bin/env bash
# requires: gcc
# 791_a_prepare_action_that_creates_nothing_fails.sh — design §5.4 P, SPEC-007
# R3.3: "when the command succeeds and the directory does not exist or holds
# no file other than the action's stamps, the engine writes no stamp and fails
# the edge, naming the directory." A
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

write_program() {   # $1 = where the stamp lies: beside | inside
    cat > build.mcpp <<EOF
import mcpp;
#include <string>
int main() {
    const std::string prefix = std::string(mcpp::out_dir()) + "/never-created";
    const std::string stamp = std::string("$1") == "inside"
        ? prefix + "/installed.stamp" : prefix + ".stamp";
    mcpp::action a;
    a.id   = "empty:prepare";
    a.role = mcpp::roles::prepare;
    a.arg("true").output(stamp.c_str()).output_dir(prefix.c_str()).submit();
    return 0;
}
EOF
}

MCPP="${MCPP:-mcpp}"

# Two legs. BESIDE: the directory is never created. INSIDE: ninja creates the
# stamp's parent, which IS the directory, before the command runs, so the
# directory exists and holds nothing the command wrote; a post-condition that
# asked only whether it exists would pass there and defend nothing.
for where in beside inside; do
    rm -rf target
    write_program "$where"
    if "$MCPP" build > b.log 2>&1; then
        cat b.log
        echo "FAIL ($where): a prepare action whose command created nothing let the build succeed"
        exit 1
    fi
    grep -qF "never-created" b.log \
        || { cat b.log; echo "FAIL ($where): the failure does not name the declared directory"; exit 1; }
    grep -qi "does not exist or holds no file" b.log \
        || { cat b.log; echo "FAIL ($where): the failure does not state the post-condition"; exit 1; }
    # No half-satisfied edge: the stamp must not exist either, or a later
    # build would read a check that "passed" without its post-condition.
    if find target -name '*.stamp' -path '*never-created*' | grep -q .; then
        echo "FAIL ($where): a stamp was written although the directory holds nothing"; exit 1
    fi
    echo "  ok: the stamp $where the directory, and the empty directory fails the edge"
done

echo "PASS: 791_a_prepare_action_that_creates_nothing_fails"
