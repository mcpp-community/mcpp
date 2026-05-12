#!/usr/bin/env bash
set -euo pipefail

# Test: workspace with two library members and one binary member.
# Verifies:
#   1. `mcpp build` at workspace root builds all members
#   2. Path deps between members work
#   3. Virtual workspace (no [package]) works

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── Create workspace structure ──────────────────────────
mkdir -p libs/core/src libs/greeter/src apps/hello/src

# Workspace root (virtual — no [package])
cat > mcpp.toml << 'EOF'
[workspace]
members = ["libs/core", "libs/greeter", "apps/hello"]
EOF

# libs/core — a simple library
cat > libs/core/mcpp.toml << 'EOF'
[package]
namespace = "demo"
name      = "core"
version   = "0.1.0"

[targets.core]
kind = "lib"
EOF

cat > libs/core/src/core.cppm << 'EOF'
export module demo.core;
import std;

export namespace demo::core {
    inline std::string greet_target() { return "World"; }
}
EOF

# libs/greeter — depends on core via path
cat > libs/greeter/mcpp.toml << 'EOF'
[package]
namespace = "demo"
name      = "greeter"
version   = "0.1.0"

[targets.greeter]
kind = "lib"

[dependencies]
core = { path = "../core" }
EOF

cat > libs/greeter/src/greeter.cppm << 'EOF'
export module demo.greeter;
import std;
import demo.core;

export namespace demo::greeter {
    inline std::string greet() {
        return "Hello, " + demo::core::greet_target() + "!";
    }
}
EOF

# apps/hello — binary that uses greeter
cat > apps/hello/mcpp.toml << 'EOF'
[package]
namespace = "demo"
name      = "hello"
version   = "0.1.0"

[dependencies]
greeter = { path = "../../libs/greeter" }
EOF

cat > apps/hello/src/main.cpp << 'EOF'
import std;
import demo.greeter;

int main() {
    std::println("{}", demo::greeter::greet());
    return 0;
}
EOF

# ── Build from workspace root ───────────────────────────
echo "=== Building from workspace root ==="
"$MCPP" build
echo "workspace build: ok"

# ── Verify the binary runs correctly ────────────────────
BIN=$(find target -type f -name hello | head -1)
test -n "$BIN" || { echo "FAIL: hello binary not found"; exit 1; }
OUT=$("$BIN" 2>&1)
echo "output: $OUT"
test "$OUT" = "Hello, World!" || { echo "FAIL: unexpected output '$OUT'"; exit 1; }
echo "workspace run: ok"

echo "ALL WORKSPACE TESTS PASSED"
