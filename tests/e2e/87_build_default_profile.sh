#!/usr/bin/env bash
# 87_build_default_profile.sh — profile selection follows the mainstream convention:
# the GLOBAL default is "dev" (-O0 -g, like Cargo/Meson/CMake/Zig/Bazel/MSBuild);
# "release" is opt-in via --release / --profile release. A project can set its own
# default with `[build] default-profile = "<name>"` (e.g. opt back into release).
# Precedence: --profile/--release/--dev flag > [build].default-profile > "dev".
# See .agents/docs/2026-06-29-manifest-environment-and-platform-design.md.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# --- (1) A plain project: no [build].default-profile → GLOBAL default = dev. ---
mkdir -p plain/src
cat > plain/mcpp.toml <<'EOF'
[package]
name    = "plain"
version = "0.1.0"
EOF
echo 'int main() { return 0; }' > plain/src/main.cpp
( cd plain
  "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build errored"; exit 1; }
  grep -q '\-O0' compile_commands.json || { echo "FAIL: global default is not dev (-O0)"; cat compile_commands.json; exit 1; }
  grep -q '\-g\b' compile_commands.json || { echo "FAIL: global default dev lacks -g"; exit 1; }
  # --release opts into the optimized profile.
  "$MCPP" build --release > b2.log 2>&1 || { cat b2.log; echo "FAIL: --release errored"; exit 1; }
  grep -q '\-O2' compile_commands.json || { echo "FAIL: --release did not yield -O2"; cat compile_commands.json; exit 1; }
)

# --- (2) A project that opts into release via [build].default-profile. ---
mkdir -p opt/src
cat > opt/mcpp.toml <<'EOF'
[package]
name    = "opt"
version = "0.1.0"
[build]
default-profile = "release"
EOF
echo 'int main() { return 0; }' > opt/src/main.cpp
( cd opt
  "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build errored"; exit 1; }
  grep -q '\-O2' compile_commands.json || { echo "FAIL: [build].default-profile=release did not yield -O2"; cat compile_commands.json; exit 1; }
  # --dev overrides the project's release default back to -O0.
  "$MCPP" build --dev > b2.log 2>&1 || { cat b2.log; echo "FAIL: --dev errored"; exit 1; }
  grep -q '\-O0' compile_commands.json || { echo "FAIL: --dev did not override project default to -O0"; cat compile_commands.json; exit 1; }
)

echo "OK"
