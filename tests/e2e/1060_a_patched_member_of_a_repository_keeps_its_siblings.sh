#!/usr/bin/env bash
# requires: gcc unix-shell
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1060 -- a patched member of a git repository keeps its siblings (design 2026-11-10 E6; mcpp#911
# N5; docs/05).
#
# A repository's root manifest lists members; one member reaches another by `path = "../core"`.
# Patching the first through a git dependency used to copy the member alone, so its `path` edge
# named a directory the copy did not have. The variant is now a copy of the repository, the patch
# names files from the member's root, and the sibling edge holds.
#
# Criteria: the build with the patch succeeds; the program prints the patched value (2) beside
# the sibling's (40).
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
repo="$t/repo"; mkdir -p "$repo/core/src" "$repo/front/src" "$t/app/src" "$t/app/patches"
cat > "$repo/mcpp.toml" <<'TOML'
[workspace]
members = ["core", "front"]
TOML
cat > "$repo/core/mcpp.toml" <<'TOML'
[package]
name    = "toy-core"
version = "0.1.0"

[targets.toy-core]
kind = "lib"
TOML
echo 'int core_value(void) { return 40; }' > "$repo/core/src/core.c"
cat > "$repo/front/mcpp.toml" <<'TOML'
[package]
name    = "toy-front"
version = "0.1.0"

[dependencies]
toy-core = { path = "../core" }

[targets.toy-front]
kind = "lib"
TOML
echo 'int front_value(void) { return 1; }' > "$repo/front/src/front.c"
(cd "$repo" && git init -q && git add -A && git -c user.name=t -c user.email=t@t commit -qm base && git tag v1)
# The patch, from the member's root: front_value returns 2.
cp -R "$repo/front" "$t/front-edit"
(cd "$t/front-edit" && rm -rf .git && git init -q && git add -A && git -c user.name=t -c user.email=t@t commit -qm b \
  && sed -i 's/return 1;/return 2;/' src/front.c && git diff) > "$t/app/patches/0001-front.patch"
grep -q -- '--- a/src/front.c' "$t/app/patches/0001-front.patch" || fail "git wrote no member-rooted patch"
cat > "$t/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[dependencies]
toy-front = { git = "file://$repo", tag = "v1", patches = ["patches/0001-front.patch"] }

[targets.app]
kind = "bin"
main = "src/main.c"
TOML
cat > "$t/app/src/main.c" <<'C'
#include <stdio.h>
int front_value(void); int core_value(void);
int main(void) { printf("%d %d\n", front_value(), core_value()); return 0; }
C
cd "$t/app"
out=$("$MCPP" build 2>&1) || fail "the patched member did not build" "$out"
got=$("$(find target -path '*/bin/app' -type f | head -1)")
[ "$got" = "2 40" ] || fail "the program printed '$got', not '2 40'"
echo "PASS: 1060 a patched member of a repository keeps its siblings"
