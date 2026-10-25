#!/usr/bin/env bash
# 937 -- the project-root compile database is published where the project asks
# for it, and nowhere by default (design 2026-10-26 D14).
#
#   A  a plain build writes the configuration's database in its build directory
#      and none at the project root;
#   B  `[build] compile_commands = "compile_commands.json"` publishes it at the
#      root, on the full path and on the fast path (a deleted copy comes back);
#   C  a root copy an earlier build left, in a project that no longer asks for
#      one, is named once (a note), and not updated;
#   D  MCPP_COMPILE_COMMANDS overrides the manifest: "off" publishes nothing.
set -e
unset MCPP_COMPILE_COMMANDS
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null
cd app

# A
"$MCPP" build > a.log 2>&1 || fail "A: the build failed" a.log
[[ ! -e compile_commands.json ]] || fail "A: a root database was written by default"
[[ -n "$(find target -name compile_commands.json)" ]] || fail "A: no configuration database"

# B
printf '\n[build]\ncompile_commands = "compile_commands.json"\n' >> mcpp.toml
"$MCPP" build > b.log 2>&1 || fail "B: the build failed" b.log
[[ -f compile_commands.json ]] || fail "B: the key published nothing" b.log
rm compile_commands.json
"$MCPP" build > b2.log 2>&1 || fail "B: the fast build failed" b2.log
[[ -f compile_commands.json ]] || fail "B: the fast path did not publish the asked copy" b2.log

# C
cp compile_commands.json keep.json
sed -i '/^compile_commands = /d' mcpp.toml
"$MCPP" build > c.log 2>&1 || fail "C: the build failed" c.log
grep -q "is no longer updated" c.log || fail "C: the left copy was not named" c.log
touch src/main.cpp
"$MCPP" build > c2.log 2>&1 || fail "C: the second build failed" c2.log
grep -q "is no longer updated" c2.log && fail "C: named twice" c2.log
cmp -s compile_commands.json keep.json || fail "C: the left copy was updated"

# D
rm -f compile_commands.json
printf '\n[build]\ncompile_commands = "compile_commands.json"\n' >> mcpp.toml
touch src/main.cpp
MCPP_COMPILE_COMMANDS=off "$MCPP" build > d.log 2>&1 || fail "D: the build failed" d.log
[[ ! -e compile_commands.json ]] || fail "D: MCPP_COMPILE_COMMANDS=off published a copy" d.log

echo "PASS: 937 the root compile database is opt-in"
