#!/usr/bin/env bash
# requires: elf
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1045 -- a required capability takes its package's default provider (mcpp#911 G7; SPEC-002 §3.6).
#
# A package requires a capability (`requires = ["acme:greeter"]`) and states its default providers
# per target (`[defaults."acme:greeter"] linux = ..., "*" = ...`). With no provider in the graph,
# the default for the target is the package's dependency; a selector that names the target wins
# over `*`. A provider the consumer brings is used instead, and no default is added.
#
# Criteria: the build reports the default; the program returns the default's value (1, Linux);
# with the consumer depending on the other provider it returns that one's (2) and no default is
# reported.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/greet-a/src" "$t/greet-b/src" "$t/lib/src" "$t/app/src"
for p in a b; do
cat > "$t/greet-$p/mcpp.toml" <<EOF
[package]
name     = "greet-$p"
version  = "0.1.0"
provides = ["acme:greeter"]

[targets.greet-$p]
kind = "lib"
EOF
done
echo 'extern "C" int greet() { return 1; }' > "$t/greet-a/src/a.cpp"
echo 'extern "C" int greet() { return 2; }' > "$t/greet-b/src/b.cpp"
cat > "$t/lib/mcpp.toml" <<'EOF'
[package]
name     = "lib"
version  = "0.1.0"
requires = ["acme:greeter"]

[targets.lib]
kind = "lib"

[defaults."acme:greeter"]
linux = { path = "../greet-a" }
"*"   = { path = "../greet-b" }
EOF
echo 'extern "C" int greet(); int lib_greet() { return greet(); }' > "$t/lib/src/lib.cpp"
cat > "$t/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
lib = { path = "../lib" }
EOF
echo 'int lib_greet(); int main() { return lib_greet(); }' > "$t/app/src/main.cpp"
cd "$t/app"

out=$("$MCPP" build 2>&1) || fail "the build failed" "$out"
grep -q "Default .*greet-a for acme:greeter of lib (linux)" <<<"$out" || fail "the default is not reported" "$out"
exe=$(find target -path '*/bin/app' -type f | head -1)
set +e; "$exe"; rc=$?; set -e
[ "$rc" = 1 ] || fail "the program returned $rc, not the Linux default's 1"
echo "  ok  the target's default provides the capability, over \`*\`"

printf 'greet-b = { path = "../greet-b" }\n' >> mcpp.toml
out=$("$MCPP" build 2>&1) || fail "the build with the consumer's provider failed" "$out"
grep -q "Default " <<<"$out" && fail "a default was added though the graph provides the capability" "$out"
set +e; "$exe"; rc=$?; set -e
[ "$rc" = 2 ] || fail "the program returned $rc, not the consumer's provider's 2"
echo "  ok  the consumer's provider is used, and no default is added"
echo "PASS: 1045 a required capability takes its default provider"
