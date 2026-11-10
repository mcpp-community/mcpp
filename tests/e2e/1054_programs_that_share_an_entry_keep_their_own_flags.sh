#!/usr/bin/env bash
# requires: gcc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1054 -- programs that begin at one file keep their own flags (design 2026-11-10 E2; mcpp#911 N1).
#
# Three programs name the same `main`. One states no flags, one states `defines` for its entry, one
# states `[targets.<name>.build]` for every unit it links. Each program is compiled with its own
# flags: the entry is one object for the programs that give it none, and one more under
# `obj/_entry/<target>/` for each program that does. Before, the first program's flags were
# compiled into the one object and linked into every program, without a word.
#
# Criteria: the programs print 1 10, 2 10 and 3 30; the shared object carries no FLAVOUR define.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'TOML'
[package]
name    = "twin"
version = "0.1.0"

[build]
sources = ["src/*.c", "!src/main.c"]

[targets.plain]
kind = "bin"
main = "src/main.c"

[targets.flagged]
kind    = "bin"
main    = "src/main.c"
defines = ["FLAVOUR=2"]

[targets.variant]
kind = "bin"
main = "src/main.c"

[targets.variant.build]
defines = ["FLAVOUR=3"]
TOML
cat > "$t/src/main.c" <<'C'
#include <stdio.h>
#ifndef FLAVOUR
#define FLAVOUR 1
#endif
int helper(void);
int main(void) { printf("%d %d\n", FLAVOUR, helper()); return 0; }
C
cat > "$t/src/helper.c" <<'C'
#ifndef FLAVOUR
#define FLAVOUR 1
#endif
int helper(void) { return FLAVOUR * 10; }
C
cd "$t"
out=$("$MCPP" build 2>&1) || fail "the build failed" "$out"
for pair in "plain:1 10" "flagged:2 10" "variant:3 30"; do
    name=${pair%%:*}; want=${pair#*:}
    exe=$(find target -path "*/bin/$name" -type f | head -1)
    [ -n "$exe" ] || fail "no program $name"
    got=$("$exe")
    [ "$got" = "$want" ] || fail "$name printed '$got', not '$want'"
done
ninja=$(find target -name build.ninja | head -1)
stanza=$(awk '/^build obj\/twin\/src\/main.o /{on=1; print; next} on && /^build /{exit} on{print}' "$ninja")
[ -n "$stanza" ] || fail "no shared entry object in build.ninja"
grep -q 'FLAVOUR' <<<"$stanza" && fail "the shared entry object carries a program's define" "$stanza"
echo "PASS: 1054 programs that share an entry keep their own flags"
