#!/usr/bin/env bash
# 126_default_features_opt_out.sh — consumer-side `default-features = false`
# (#242, Cargo parity). A dependency declares a DEFAULT feature set
# (`[features] default = ["turbo"]`) that carries a package-owned define. A
# consumer must be able to OPT OUT of that default set via
# `default-features = false`, so the dependency's default feature (and its
# define) does NOT activate. Explicitly requesting `features = [...]` still
# activates those, even alongside the opt-out.
#
# Root-cause funnel: the feature-activation closure (prepare.cppm
# feature_closure) no longer unconditionally seeds `[features].default` for a
# dependency — it seeds it only when the consumer's dep spec keeps default
# features (seedDefault). See the #242 commit.
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Dependency whose DEFAULT feature set enables `turbo`, which carries a
# package-owned define that propagates to consumers (header-only case).
mkdir -p widget/include/widget widget/src
cat > widget/mcpp.toml <<'EOF'
[package]
name    = "widget"
version = "0.1.0"

[features]
default = ["turbo"]
turbo   = { defines = ["WIDGET_TURBO=1"] }

[build]
include_dirs = ["include"]

[targets.widget]
kind = "lib"
EOF
cat > widget/include/widget/widget.hpp <<'EOF'
#pragma once
inline int widget_mode() {
#ifdef WIDGET_TURBO
    return 1;
#else
    return 0;
#endif
}
EOF
cat > widget/src/widget.cppm <<'EOF'
export module widget;
export int widget_anchor() { return 0; }
EOF

# ── Case A: default-features = false → the dep's default `turbo` is SUPPRESSED.
# The consumer TU must NOT see WIDGET_TURBO; if it did, the #error below trips
# and the build fails.
mkdir -p appoff/src
cat > appoff/mcpp.toml <<'EOF'
[package]
name    = "appoff"
version = "0.1.0"

[dependencies]
widget = { path = "../widget", default-features = false }
EOF
cat > appoff/src/main.cpp <<'EOF'
#include <widget/widget.hpp>
#ifdef WIDGET_TURBO
#error "WIDGET_TURBO leaked despite default-features = false"
#endif
int main() { return widget_mode() == 0 ? 0 : 2; }
EOF

( cd appoff
  "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL(A): opt-out build failed"; exit 1; }
  if grep -q 'WIDGET_TURBO' compile_commands.json; then
      echo "FAIL(A): WIDGET_TURBO present despite default-features = false"; cat compile_commands.json; exit 1
  fi
  BIN=$(find target -type f \( -name appoff -o -name appoff.exe \) | head -1)
  [ -n "$BIN" ] || { echo "FAIL(A): built binary not found"; exit 1; }
  "$BIN"; rc=$?
  [ "$rc" -eq 0 ] || { echo "FAIL(A): binary observed turbo despite opt-out (exit $rc)"; exit 1; }
)

# ── Case B: default features ON (omitted) → the dep's default `turbo` activates
# and its define reaches the consumer, exactly as before this change.
mkdir -p appon/src
cat > appon/mcpp.toml <<'EOF'
[package]
name    = "appon"
version = "0.1.0"

[dependencies]
widget = { path = "../widget" }
EOF
cat > appon/src/main.cpp <<'EOF'
#include <widget/widget.hpp>
#ifndef WIDGET_TURBO
#error "WIDGET_TURBO missing though default features are on"
#endif
int main() { return widget_mode() == 1 ? 0 : 2; }
EOF

( cd appon
  "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL(B): default-on build failed"; exit 1; }
  grep -q 'WIDGET_TURBO' compile_commands.json || {
      echo "FAIL(B): WIDGET_TURBO missing with default features on"; cat compile_commands.json; exit 1; }
)

# ── Case C: default-features = false BUT explicitly re-request `turbo` →
# the explicit request still activates it (opt-out suppresses only the default
# seed, not explicit `features = [...]`).
mkdir -p appre/src
cat > appre/mcpp.toml <<'EOF'
[package]
name    = "appre"
version = "0.1.0"

[dependencies]
widget = { path = "../widget", default-features = false, features = ["turbo"] }
EOF
cat > appre/src/main.cpp <<'EOF'
#include <widget/widget.hpp>
#ifndef WIDGET_TURBO
#error "explicit features = [turbo] did not activate alongside default-features = false"
#endif
int main() { return widget_mode() == 1 ? 0 : 2; }
EOF

( cd appre
  "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL(C): opt-out + explicit build failed"; exit 1; }
  grep -q 'WIDGET_TURBO' compile_commands.json || {
      echo "FAIL(C): explicit feature did not activate alongside opt-out"; cat compile_commands.json; exit 1; }
)

echo "OK"
