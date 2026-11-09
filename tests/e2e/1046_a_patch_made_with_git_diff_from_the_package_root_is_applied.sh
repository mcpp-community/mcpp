#!/usr/bin/env bash
# requires: elf gcc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1046 -- a patch made with `git diff` from the package's root is applied (mcpp#911 G8; docs/05).
#
# A patch names files from the package's root, the directory of its resolved manifest, which
# `mcpp why deps` reports. A copy of that tree under git, edited, gives the patch with
# `git diff`; the edge's `patches` lists it, and the build reads the patched variant, whose root
# `mcpp why deps` then reports.
#
# Criteria: `mcpp why deps` reports the dependency's root; the patch git writes from a copy of it
# applies; the variant holds the change and is the root reported afterwards.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/app/src"
cd "$t/app"
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mcpplibs.cmdline = "0.0.1"
EOF
echo 'int main() { return 0; }' > src/main.cpp

out=$("$MCPP" why deps 2>&1) || fail "why deps failed" "$out"
root=$(sed -n '/^  mcpplibs.cmdline[@+]/,/^  [^ ]/s/^      root: //p' <<<"$out" | head -1)
[ -n "$root" ] && [ -d "$root" ] || fail "the dependency's root is not reported" "$out"
echo "  ok  why deps reports the root"

cp -R "$root" "$t/cmdline"
file=$(cd "$t/cmdline" && find . -name parse.cppm | head -1); file=${file#./}
[ -n "$file" ] || fail "the root holds no parse.cppm" "$(ls -R "$t/cmdline" | head)"
(cd "$t/cmdline" && git init -q && git add -A && git -c user.name=t -c user.email=t@t commit -qm base \
  && echo '// changed for the patch' >> "$file")
mkdir -p patches
(cd "$t/cmdline" && git diff) > patches/0001-change.patch
grep -q -- "--- a/$file" patches/0001-change.patch || fail "git did not name the file from the root" "$(head -3 patches/0001-change.patch)"
sed -i.bak 's|^mcpplibs.cmdline = "0.0.1"$|mcpplibs.cmdline = { version = "0.0.1", patches = ["patches/0001-change.patch"] }|' mcpp.toml

out=$("$MCPP" build 2>&1) || fail "the patched build failed" "$out"
out=$("$MCPP" why deps 2>&1) || fail "why deps failed after the patch" "$out"
variant=$(sed -n '/^  mcpplibs.cmdline[@+]/,/^  [^ ]/s/^      root: //p' <<<"$out" | head -1)
[ -n "$variant" ] && [ "$variant" != "$root" ] || fail "the variant is not the reported root" "$out"
grep -q 'changed for the patch' "$variant/$file" || fail "the variant does not hold the change"
echo "  ok  the patch applies and the variant is the root the build reads"
echo "PASS: 1046 a patch made with git diff from the package root is applied"
