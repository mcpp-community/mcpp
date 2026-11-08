#!/usr/bin/env bash
# requires: elf gcc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1046 -- a dependency is edited and its change saved as its patch (mcpp#911 G8; docs/05).
#
# `mcpp patch edit <dependency>` copies the dependency's package root, as the build reads it, to
# target/patch/<dependency>/. `mcpp patch save <dependency>` writes the difference as the edge's
# next patch under patches/, named from the package's root, and adds it to the edge; the build
# then reads the patched variant.
#
# Criteria: the patch names the edited file from the package's root; mcpp.toml's edge lists it;
# the build succeeds and its variant tree holds the change; a second save without a change writes
# nothing.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mcpplibs.cmdline = "0.0.1"
EOF
echo 'int main() { return 0; }' > "$t/src/main.cpp"
cd "$t"

out=$("$MCPP" patch edit cmdline 2>&1) || fail "patch edit failed" "$out"
grep -q "Editing cmdline at .*target/patch/mcpplibs.cmdline" <<<"$out" || fail "the edit tree is not reported" "$out"
file=$(find target/patch/mcpplibs.cmdline -name parse.cppm | head -1)
[ -n "$file" ] || fail "the edit tree holds no parse.cppm" "$(find target/patch -maxdepth 3)"
rel=${file#target/patch/mcpplibs.cmdline/}
echo '// changed through mcpp patch' >> "$file"

out=$("$MCPP" patch save cmdline 2>&1) || fail "patch save failed" "$out"
grep -q "Saved patches/cmdline-0001.patch" <<<"$out" || fail "the patch is not saved" "$out"
grep -q -- "--- a/$rel" patches/cmdline-0001.patch || fail "the patch does not name the file from the package root" "$(head -3 patches/cmdline-0001.patch)"
grep -q 'patches = \["patches/cmdline-0001.patch"\]' mcpp.toml || fail "the edge does not list the patch" "$(cat mcpp.toml)"
[ ! -e target/patch/mcpplibs.cmdline ] || fail "the edit tree is left behind"
echo "  ok  the change is saved as the edge's patch, named from the package root"

out=$("$MCPP" build 2>&1) || fail "the patched build failed" "$out"
home="${MCPP_HOME:-$HOME/.mcpp}"
grep -rqs 'changed through mcpp patch' "$home"/cache/src/mcpplibs.cmdline/*/"$rel" || fail "no variant holds the change"
echo "  ok  the build reads the patched variant"

"$MCPP" patch edit cmdline >/dev/null 2>&1 || fail "the second edit failed"
out=$("$MCPP" patch save cmdline 2>&1) || fail "the second save failed" "$out"
grep -q "Unchanged" <<<"$out" || fail "a save without a change wrote something" "$out"
[ ! -e patches/cmdline-0002.patch ] || fail "a second patch was written"
echo "  ok  a save without a change writes nothing"
echo "PASS: 1046 a dependency is edited and its change saved as a patch"
