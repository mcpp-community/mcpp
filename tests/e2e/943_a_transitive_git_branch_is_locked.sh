#!/usr/bin/env bash
# requires: gcc
# 943 -- a git branch reached through another package is locked as the root's
# own are.
#
# mcpp.lock recorded the commit of a branch dependency the ROOT declared, and
# read it back as the dependency's anchor; a branch two edges away was not
# written, so every build asked `git ls-remote` again and moved to the tip, and
# an `--offline` build stopped at it. Measured in mcpp's own CI, whose platform
# member depends on mcpplibs.os by branch and os on openkal by branch:
#
#   offline mode: git dependency 'openkal' needs `git ls-remote` ...
#          mcpp.lock records no commit for branch 'openkal-0.17.0'
#
# Criteria:
#   A. After the first build mcpp.lock holds the transitive branch at the commit
#      the build used.
#   B. A commit pushed to that branch afterwards is not taken by the next build:
#      the program still prints what the locked commit says. (A `file://`
#      remote is local, so B rather than `--offline` is what shows the lock line
#      is read back as the anchor.)
#   C. `mcpp update` drops the recorded commit, and the next build takes the
#      branch's tip: the record of the previous build is not replayed under a
#      lock it was not planned with (2026.10.19.1 printed the old value on every
#      run after the update).
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

repo() {   # repo <dir>: a git repository with one commit of what is there
    (cd "$1" && git init --quiet -b main && git config user.email t@local \
        && git config user.name t && git add -A && git commit --quiet -m init)
}

# leaf: the transitive dependency, on branch main.
mkdir -p "$TMP/leaf/src"
cat > "$TMP/leaf/mcpp.toml" <<'TOML'
[package]
name    = "leaf"
version = "0.1.0"

[targets.leaf]
kind = "lib"
TOML
printf 'export module leaf;\nexport int leaf_value() { return 1; }\n' > "$TMP/leaf/src/leaf.cppm"
repo "$TMP/leaf"

# mid: depends on leaf by branch.
mkdir -p "$TMP/mid/src"
cat > "$TMP/mid/mcpp.toml" <<TOML
[package]
name    = "mid"
version = "0.1.0"

[targets.mid]
kind = "lib"

[dependencies]
leaf = { git = "file://$TMP/leaf", branch = "main" }
TOML
printf 'export module mid;\nimport leaf;\nexport int mid_value() { return leaf_value(); }\n' > "$TMP/mid/src/mid.cppm"
repo "$TMP/mid"

# app: depends on mid by branch; leaf is two edges away.
mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
mid = { git = "file://$TMP/mid", branch = "main" }
TOML
printf 'import mid;\n#include <cstdio>\nint main() { std::printf("value %%d\\n", mid_value()); }\n' > src/main.cpp

"$MCPP" build > b1.log 2>&1 || fail "the first build" b1.log
LEAF_COMMIT=$(git -C "$TMP/leaf" rev-parse HEAD)

# A
grep -q "branch=main@$LEAF_COMMIT" mcpp.lock || fail "A: mcpp.lock does not hold the transitive branch's commit" mcpp.lock
echo "ok: the transitive branch is locked at the commit the build used"

# B
printf 'export module leaf;\nexport int leaf_value() { return 2; }\n' > "$TMP/leaf/src/leaf.cppm"
(cd "$TMP/leaf" && git commit --quiet -am two)
"$MCPP" build > b2.log 2>&1 || fail "B: the second build" b2.log
"$MCPP" run > run.log 2>&1 || fail "B: the program did not run" run.log
grep -q "value 1" run.log || fail "B: the build moved to the branch's new tip" run.log mcpp.lock
echo "ok: a later commit on the branch is not taken"

# C
"$MCPP" update > update.log 2>&1 || fail "C: mcpp update" update.log
"$MCPP" run > run2.log 2>&1 || fail "C: the program did not run after the update" run2.log
grep -q "value 2" run2.log || fail "C: the build after mcpp update did not take the branch's tip" run2.log
echo "ok: after mcpp update the next build takes the branch's tip"

echo "PASS: 943 a transitive git branch is locked"
