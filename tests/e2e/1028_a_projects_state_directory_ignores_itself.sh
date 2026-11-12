#!/usr/bin/env bash
# 1028 -- a project's state directory ignores itself.
#
# A build writes `<project>/.mcpp/` (its compiler runs' locks; a project's own
# indices and SubOS live there too), the machine's state and never the
# project's sources. A project whose ignore file does not name it -- one not
# made by `mcpp new` -- showed it to version control, and a commit took the
# lock files (mcpplibs/os 0.4.0's first commit).
#
# Criterion: after a build in a repository whose .gitignore names only
# target/, `git status` lists nothing under .mcpp/, and `.mcpp/.gitignore` is `*`.
set -e
command -v git > /dev/null || { echo "SKIP: no git on this machine"; exit 77; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/p/src"
cd "$TMP/p"
printf '[package]\nname    = "p"\nversion = "0.1.0"\n' > mcpp.toml
printf 'int main() { return 0; }\n' > src/main.cpp
printf 'target/\n' > .gitignore
git init -q .
"$MCPP" build > build.log 2>&1 || fail "the build failed" build.log
[ -d .mcpp ] || { echo "SKIP: this build wrote no state directory"; exit 77; }
grep -qx '\*' .mcpp/.gitignore || fail ".mcpp/.gitignore does not ignore everything" .mcpp/.gitignore
git status --porcelain --untracked-files=all > status.txt
grep -q '^?? \.mcpp/' status.txt && fail "version control sees the state directory" status.txt
echo "PASS: 1028 a project's state directory ignores itself"
