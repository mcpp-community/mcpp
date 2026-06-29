#!/usr/bin/env bash
# 82_feature_optional_deps.sh — Feature System v2 Stage 2a: a dependency declared
# under a feature (`[feature-deps.<name>]`) is pulled ONLY when that feature is
# active; otherwise it is never fetched/resolved. When active it is resolved and
# built like any normal dependency. See
# .agents/docs/2026-06-29-feature-optional-dependencies-s2-design.md.
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A small path library the feature optionally pulls in.
mkdir -p widget/src
cat > widget/mcpp.toml <<'EOF'
[package]
name    = "widget"
version = "0.1.0"

[targets.widget]
kind = "lib"
EOF
cat > widget/src/widget.cppm <<'EOF'
export module widget;
export int widget_answer() { return 42; }
EOF

mkdir -p app/src
echo 'int main() { return 0; }' > app/src/main.cpp
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[features]
default = []
extra   = []

# `widget` is declared ONLY under the feature, so it is optional: resolved only
# when --features extra is active.
[feature-deps.extra]
widget = { path = "../widget" }
EOF

cd app

# 1. Feature inactive → widget is NOT pulled (never resolved/compiled).
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: baseline build failed"; exit 1; }
if grep -q 'Compiling widget' b1.log; then
    cat b1.log; echo "FAIL: widget must NOT be pulled when feature inactive"; exit 1
fi

# 2. Feature active → widget IS pulled and compiled like a normal dependency.
rm -rf target
"$MCPP" build --features extra > b2.log 2>&1 || { cat b2.log; echo "FAIL: feature build failed (widget not pulled?)"; exit 1; }
grep -q 'Compiling widget' b2.log || { cat b2.log; echo "FAIL: widget was not pulled/compiled when feature active"; exit 1; }

echo "OK"
