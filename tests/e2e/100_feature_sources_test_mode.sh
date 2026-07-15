#!/usr/bin/env bash
# requires:
# 100_feature_sources_test_mode.sh — a dependency's feature-gated `sources` must
# be compiled under `mcpp test`, not just `mcpp build`.
#
# Regression: prepare_build() gated the whole feature-source resolution (drop +
# add) on `!includeDevDeps`, so `mcpp test` never added an active feature's
# sources. Descriptors that list a glob ONLY under `features` (xpkg's
# `features.X.sources` never lands in base `sources`) therefore compiled fine
# under `mcpp build` but failed to link under `mcpp test` with `undefined
# reference` — compat.cjson's `utils`, compat.spdlog's `compiled`, and
# compat.eigen's `eigen_blas` all hit this. The drop stays build-mode only (see
# 79_gtest_regular_dep_feature_main.sh); the add now runs in both modes.
#
# compat.cjson is the vehicle: plain C, small, and `utils` (cJSON_Utils.c) is
# feature-gated with no other dependency.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
rm -f src/main.cpp
mkdir -p tests

cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
cjson = { version = "1.7.19", features = ["utils"] }
EOF

# Calls into BOTH the base sources (cJSON.c) and the feature-gated ones
# (cJSON_Utils.c) — a link failure on either fails the test.
cat > tests/t.cpp <<'EOF'
#include <cJSON.h>
#include <cJSON_Utils.h>
int main() {
    cJSON* root = cJSON_Parse("{\"a\":1}");
    if (!root) return 1;
    char* p = cJSONUtils_FindPointerFromObjectTo(root, root);
    int ok = (p != 0);
    cJSON_Delete(root);
    return ok ? 0 : 1;
}
EOF

# (1) THE REGRESSION: `mcpp test` must compile the feature's sources and link.
"$MCPP" test > /dev/null 2>&1 || {
    echo "FAIL: feature-gated sources not compiled under \`mcpp test\`"
    "$MCPP" test 2>&1 | tail -5
    exit 1
}
nj=$(find target -name build.ninja | xargs ls -t 2>/dev/null | head -1)
grep -q 'cJSON_Utils' "$nj" || {
    echo "FAIL: cJSON_Utils.c absent from the test-mode build graph"; exit 1
}

# (2) `mcpp build` keeps working (the path that was always correct).
cat > src/main.cpp <<'EOF'
#include <cJSON.h>
#include <cJSON_Utils.h>
int main() {
    cJSON* root = cJSON_Parse("{\"a\":1}");
    char* p = cJSONUtils_FindPointerFromObjectTo(root, root);
    (void)p; cJSON_Delete(root); return 0;
}
EOF
"$MCPP" build > /dev/null || { echo "FAIL: build with feature sources failed"; exit 1; }
nj=$(find target -name build.ninja | xargs ls -t 2>/dev/null | head -1)
grep -q 'cJSON_Utils' "$nj" || { echo "FAIL: cJSON_Utils.c absent from build graph"; exit 1; }

# (3) The drop still works: without the feature, the gated source stays OUT of a
#     `mcpp build` graph (guards against "add everything unconditionally").
cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
cjson = "1.7.19"
EOF
cat > src/main.cpp <<'EOF'
#include <cJSON.h>
int main() { cJSON* r = cJSON_Parse("{\"a\":1}"); cJSON_Delete(r); return 0; }
EOF
"$MCPP" build > /dev/null || { echo "FAIL: default (no feature) build failed"; exit 1; }
nj=$(find target -name build.ninja | xargs ls -t 2>/dev/null | head -1)
if grep -q 'cJSON_Utils' "$nj"; then
    echo "FAIL: cJSON_Utils.c compiled without the \`utils\` feature"; exit 1
fi

echo "OK"
