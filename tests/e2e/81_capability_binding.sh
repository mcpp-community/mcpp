#!/usr/bin/env bash
# 81_capability_binding.sh — Feature System v2 Stage 3: capabilities.
# A package/feature may `requires = ["cap"]` an abstract capability; providers
# declare `provides = ["cap"]`. The resolver binds exactly ONE provider from the
# dependency graph:
#   0 providers          → hard error "no package provides"
#   1 provider           → bind (success)
#   ≥2 unpinned          → hard error listing candidates
#   pinned [capabilities] → the pin wins
# See .agents/docs/2026-06-29-feature-capability-model-design.md.
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Two interchangeable BLAS providers.
for impl in alpha beta; do
    mkdir -p "blas_$impl/src"
    cat > "blas_$impl/mcpp.toml" <<EOF
[package]
name    = "blas_$impl"
version = "0.1.0"
provides = ["blas"]

[targets.blas_$impl]
kind = "lib"
EOF
    printf 'export module blas_%s;\nexport int blas_%s() { return 0; }\n' "$impl" "$impl" > "blas_$impl/src/blas_$impl.cppm"
done

mkdir -p app/src
echo 'int main() { return 0; }' > app/src/main.cpp
cd app

write_manifest() {  # $1 = dependency lines
    cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[features]
default  = []
use_blas = { requires = ["blas"] }

[dependencies]
$1
EOF
}

# Case 1: exactly one provider in the graph + feature active → binds, builds.
write_manifest 'blas_alpha = { path = "../blas_alpha" }'
rm -rf target
"$MCPP" build --features use_blas > c1.log 2>&1 || { cat c1.log; echo "FAIL case1: single provider should bind"; exit 1; }

# Case 2: capability required but NO provider in the graph → hard error.
write_manifest '# no provider'
rm -rf target
if "$MCPP" build --features use_blas > c2.log 2>&1; then
    cat c2.log; echo "FAIL case2: missing provider must error"; exit 1
fi
grep -qi "no package provides .*blas\|capability 'blas'" c2.log || { cat c2.log; echo "FAIL case2: wrong/no error text"; exit 1; }

# Case 3: two providers, unpinned → ambiguity error listing candidates.
write_manifest 'blas_alpha = { path = "../blas_alpha" }
blas_beta  = { path = "../blas_beta" }'
rm -rf target
if "$MCPP" build --features use_blas > c3.log 2>&1; then
    cat c3.log; echo "FAIL case3: ambiguous providers must error"; exit 1
fi
grep -qi "blas_alpha" c3.log && grep -qi "blas_beta" c3.log || { cat c3.log; echo "FAIL case3: error must list candidates"; exit 1; }

# Case 4: same two providers, pinned via [capabilities] → the pin wins, builds.
cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[features]
default  = []
use_blas = { requires = ["blas"] }

[capabilities]
blas = "blas_beta"

[dependencies]
blas_alpha = { path = "../blas_alpha" }
blas_beta  = { path = "../blas_beta" }
EOF
rm -rf target
"$MCPP" build --features use_blas > c4.log 2>&1 || { cat c4.log; echo "FAIL case4: pin should resolve ambiguity"; exit 1; }

# Case 5: feature inactive → no requirement, no provider needed, builds clean.
write_manifest '# no provider'
rm -rf target
"$MCPP" build > c5.log 2>&1 || { cat c5.log; echo "FAIL case5: inactive feature must not require capability"; exit 1; }

# Case 6: two providers, --cap pin on the CLI resolves the ambiguity.
write_manifest 'blas_alpha = { path = "../blas_alpha" }
blas_beta  = { path = "../blas_beta" }'
rm -rf target
"$MCPP" build --features use_blas --cap blas=blas_alpha > c6.log 2>&1 || { cat c6.log; echo "FAIL case6: --cap pin should resolve ambiguity"; exit 1; }

echo "OK"
