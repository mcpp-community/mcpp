#!/usr/bin/env bash
# requires: gcc
# Per-target required_features gate (issue #131): a target is emitted only when
# all its required features are active. `--features X` builds only the matching
# target; the gated-out target's entry is never compiled (no main() clash).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p proj/src proj/bin
cd proj

# Shared code (globbed). Entry mains live outside src/ so a gated-out target's
# main is not globbed into the shared pool.
cat > src/common.cpp <<'EOF'
extern "C" int shared_answer(void) { return 0; }
EOF
cat > bin/server.cpp <<'EOF'
extern "C" int shared_answer(void);
int main() { return shared_answer(); }
EOF
cat > bin/client.cpp <<'EOF'
extern "C" int shared_answer(void);
int main() { return shared_answer(); }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "proj"
version = "0.1.0"

[build]
sources = ["src/**/*.cpp"]

[features]
server = []
client = []

[targets.server]
kind = "bin"
main = "bin/server.cpp"
required_features = ["server"]

[targets.client]
kind = "bin"
main = "bin/client.cpp"
required_features = ["client"]
EOF

binpath() { find target -path '*/bin/'"$1" 2>/dev/null | head -1; }

# --features server → only the server target is emitted.
"$MCPP" build --features server > s.log 2>&1 || { cat s.log; echo "server build failed"; exit 1; }
[[ -n "$(binpath server)" ]] || { echo "server binary not built under --features server"; exit 1; }
[[ -z "$(binpath client)" ]] || { echo "client binary should be gated out under --features server"; exit 1; }

# --features client → only the client target is emitted.
"$MCPP" build --features client > c.log 2>&1 || { cat c.log; echo "client build failed"; exit 1; }
[[ -n "$(binpath client)" ]] || { echo "client binary not built under --features client"; exit 1; }

echo "OK"
