#!/usr/bin/env bash
# requires:
# 161_xpkg_name_form.sh — INV-NAME (#278): `package.name` must BE the
# fully-qualified name whenever `package.namespace` is declared.
#
# Why this matters: the package index is a FLAT key space keyed by the literal
# `package.name`, while mcpp addresses a dependency as `ns + "." + shortName`.
# A split-form descriptor (namespace="a", name="b") satisfies mcpp's identity
# gate and prints fine from `xpkg parse`, but the two never meet — the package
# is uninstallable on every platform, and before this check the user found out
# an hour into a three-platform CI run via an opaque E_NOT_FOUND.
#
# Covered here: the lint gate (`mcpp xpkg parse`), its `--json` shape, the
# `--allow-split-name` escape for xlings-native indices, and the runtime
# fail-fast that catches descriptors which never passed through index CI.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── 1. split form is rejected by the lint, and says how to fix it ────
cat > split.lua <<'EOF'
package = {
    spec = "1", namespace = "chriskohlhoff", name = "asio",
    xpm = { linux = { ["1.38.1"] = { url = "u", sha256 = "h" } } },
    mcpp = { schema = "0.1", sources = { "*.cpp" } },
}
EOF

if "$MCPP" xpkg parse split.lua > /dev/null 2>&1; then
    echo "FAIL: split-form descriptor must be rejected"; exit 1
fi
"$MCPP" xpkg parse split.lua 2>&1 | tee split.err
grep -q "fully-qualified" split.err
# The diagnostic must carry the exact spelling to write — a bare "invalid"
# would leave the author guessing which half of the identity to change.
grep -q 'chriskohlhoff.asio' split.err

# ── 2. the FQN form passes ───────────────────────────────────────────
sed 's/name = "asio"/name = "chriskohlhoff.asio"/' split.lua > fqn.lua
"$MCPP" xpkg parse fqn.lua > /dev/null

# ── 3. --json carries the violation machine-readably (index CI) ──────
if "$MCPP" xpkg parse --json split.lua > split.json 2>/dev/null; then
    echo "FAIL: --json must still exit non-zero on violation"; exit 1
fi
python3 - <<'PY'
import json
j = json.load(open("split.json"))
assert "error" in j, j
assert "chriskohlhoff.asio" in j["error"], j
assert j["namespace"] == "chriskohlhoff" and j["name"] == "asio", j
print("json ok")
PY

# ── 4. --allow-split-name opts out (xlings-native indices) ───────────
# In xim-pkgindex / -scode, `package.namespace` is an install-dir CATEGORY
# ("config", "scode") and the index is keyed by the bare name, so the split
# spelling is correct there. Those trees lint with this flag.
"$MCPP" xpkg parse --allow-split-name split.lua > /dev/null

# ── 5. runtime fail-fast: a descriptor that skipped index CI ─────────
# A `[indices]` path index is the transport that bypasses lint entirely, so
# the same predicate has to guard the install path. It must fire BEFORE any
# download, from the descriptor mcpp already holds in memory.
mkdir -p idx/pkgs/d
cat > idx/pkgs/d/demo.thing.lua <<'EOF'
package = {
    spec = "1", namespace = "demo", name = "thing",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            macosx = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            windows = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { schema = "0.1", sources = { "*.cpp" } },
}
EOF

mkdir -p app/src && cd app
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[indices]
demo = { path = "../idx" }

[dependencies.demo]
thing = "1.0.0"
EOF
echo 'int main() { return 0; }' > src/main.cpp

if "$MCPP" build > build.out 2>&1; then
    echo "FAIL: split-form descriptor must not build"; cat build.out; exit 1
fi
grep -q "fully-qualified" build.out || { cat build.out; exit 1; }
grep -q "demo.thing" build.out || { cat build.out; exit 1; }
# It must fail at resolve time, never after fetching the (nonexistent) asset.
if grep -q "Downloading" build.out; then
    echo "FAIL: must fail before any download"; cat build.out; exit 1
fi

echo "PASS 161_xpkg_name_form"
