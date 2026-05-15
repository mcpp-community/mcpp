#!/usr/bin/env bash
# Custom [indices] parsing: a local path index is parsed from mcpp.toml
# and visible in `mcpp index list`. Verifies the TOML parsing path for
# short form, long form, and local path indices without requiring any
# network access.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

# ── 1. Create a fake local index directory ──────────────────────────────
INDEX_DIR="$TMP/my-local-index"
mkdir -p "$INDEX_DIR/pkgs/t"
cat > "$INDEX_DIR/pkgs/t/test-pkg.lua" <<'EOF'
package = {
    homepage = "https://example.com",
    description = "A test package for E2E testing",
    license = "MIT",
}
xpm = {
    linux = {
        ["1.0.0"] = {
            url = "https://example.com/test-pkg-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
        },
    },
}
EOF

# ── 2. Create a project with [indices] section ─────────────────────────
mkdir -p "$TMP/project"
cd "$TMP/project"
"$MCPP" new myapp > /dev/null
cd myapp

cat > mcpp.toml <<EOF
[package]
name    = "myapp"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR" }
acme = "git@gitlab.example.com:platform/mcpp-index.git"
acme-stable = { url = "git@gitlab.example.com:stable.git", tag = "v2.0" }

[targets.myapp]
kind = "bin"
main = "src/main.cpp"
EOF

# ── 3. Verify `mcpp index list` shows the custom indices ───────────────
out=$("$MCPP" index list 2>&1) || true

# Project indices should appear in the output
[[ "$out" == *"local-dev"* ]]    || { echo "missing local-dev in output: $out"; exit 1; }
[[ "$out" == *"local path"* ]]   || { echo "missing 'local path' tag: $out"; exit 1; }
[[ "$out" == *"acme"* ]]         || { echo "missing acme in output: $out"; exit 1; }
[[ "$out" == *"acme-stable"* ]]  || { echo "missing acme-stable in output: $out"; exit 1; }
[[ "$out" == *"tag: v2.0"* ]]    || { echo "missing tag annotation: $out"; exit 1; }

echo "OK"
