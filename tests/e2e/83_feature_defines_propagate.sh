#!/usr/bin/env bash
# 83_feature_defines_propagate.sh — Feature System v2: a dependency's active-
# feature `defines` are INTERFACE requirements. When a consumer enables a feature
# on a (header-only) dependency, that feature's `defines` must reach the
# CONSUMER's own translation units — not only the dependency's own compile. This
# is the header-only-library case (e.g. Eigen's `use_blas` → EIGEN_USE_BLAS,
# which only changes behavior in the TU that includes Eigen's headers). The
# define propagates along Public/Interface dependency edges, mirroring
# include_dirs. See .agents/docs/2026-06-29-feature-capability-model-design.md.
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Header-only dependency: a feature `turbo` carries a package-owned define.
mkdir -p widget/include/widget widget/src
cat > widget/mcpp.toml <<'EOF'
[package]
name    = "widget"
version = "0.1.0"

[features]
default = []
turbo   = { defines = ["WIDGET_TURBO=1"] }

[build]
include_dirs = ["include"]

[targets.widget]
kind = "lib"
EOF
cat > widget/include/widget/widget.hpp <<'EOF'
#pragma once
// The macro's value is only meaningful in the TU that includes this header —
// exactly the header-only library situation.
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

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
widget = { path = "../widget", features = ["turbo"] }
EOF
# The consumer's TU asserts the dependency's feature define reached it. If the
# define does NOT propagate, this fails to compile (#error), failing the build.
cat > app/src/main.cpp <<'EOF'
#include <widget/widget.hpp>
#ifndef WIDGET_TURBO
#error "WIDGET_TURBO did not propagate from widget[turbo] to the consumer"
#endif
int main() { return widget_mode() == 1 ? 0 : 2; }
EOF

cd app

# Build: widget[turbo]'s define must reach app/src/main.cpp. A missing
# propagation makes main.cpp hit the #error and the build fails.
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: feature define did not propagate to consumer"; exit 1; }

# Double-check the compile database carries the define on the consumer TU.
grep -q 'WIDGET_TURBO' compile_commands.json || {
    echo "FAIL: WIDGET_TURBO missing from consumer compile_commands.json"; cat compile_commands.json; exit 1; }

# And the produced binary observes turbo mode at runtime. (Binary name is
# platform-dependent — `app` on POSIX, `app.exe` on Windows.)
BIN=$(find target -type f \( -name app -o -name app.exe \) | head -1)
[ -n "$BIN" ] || { echo "FAIL: built binary not found under target/"; exit 1; }
"$BIN"; rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: binary did not observe WIDGET_TURBO (exit $rc)"; exit 1; }

echo "OK"
