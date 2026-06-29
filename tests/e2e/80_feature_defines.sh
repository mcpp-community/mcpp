#!/usr/bin/env bash
# 80_feature_defines.sh — a [features] entry may carry `defines` (package-owned
# preprocessor macros). When the feature is active, each define lands on the
# package's compile flags (visible in compile_commands.json); when inactive, it
# does not. This is Feature System v2 Stage 1 — see
# .agents/docs/2026-06-29-feature-capability-model-design.md.
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[features]
default = []
accel   = { defines = ["APP_ACCEL=1"] }
EOF
cat > app/src/main.cpp <<'EOF'
int main() {
#ifdef APP_ACCEL
    return 0;
#else
    return 1;
#endif
}
EOF

cd app
cdb=compile_commands.json

# 1. Feature inactive → the define must NOT appear in the compile database.
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "baseline build failed"; exit 1; }
if grep -q 'APP_ACCEL' "$cdb"; then
    echo "FAIL: APP_ACCEL present in $cdb without the feature active"; cat "$cdb"; exit 1
fi

# 2. Feature active → the define MUST appear in the compile database.
rm -rf target "$cdb"
"$MCPP" build --features accel > b2.log 2>&1 || { cat b2.log; echo "feature build failed"; exit 1; }
grep -q 'APP_ACCEL' "$cdb" || { echo "FAIL: APP_ACCEL missing from $cdb with --features accel"; cat "$cdb"; exit 1; }

# 3. The automatic MCPP_FEATURE_<NAME> define still coexists with custom defines.
grep -q 'MCPP_FEATURE_ACCEL' "$cdb" || { echo "FAIL: MCPP_FEATURE_ACCEL missing from $cdb"; cat "$cdb"; exit 1; }

echo "OK"
