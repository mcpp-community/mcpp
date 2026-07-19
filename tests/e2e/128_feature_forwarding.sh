#!/usr/bin/env bash
# 128_feature_forwarding.sh — feature dep/feat forwarding (#243, Cargo parity).
# A package feature may open a feature OF a dependency:
#
#     [features]
#     withextra = ["leaf/extra"]   # when `withextra` is active, request
#                                  # `extra` from dependency `leaf`
#
# This is what unblocks a module package's optional module interface (the
# opencv.dnn case): a single feature both pulls a source set locally AND opens
# the heavy feature of the compat dependency, without forcing every consumer to
# pay for it.
#
# Graph: app -> mid -> leaf.  `leaf` has a feature `extra` (NOT default) that
# carries a package-owned define LEAF_EXTRA. `mid` forwards `leaf/extra` behind
# its own `withextra` feature and pulls leaf with default-features = false (so
# leaf's default set never brings `extra` on its own). The forward must:
#   * activate leaf's `extra` ONLY when the consumer turns on mid/withextra,
#   * travel TRANSITIVELY (root app -> mid -> leaf),
#   * hold on BOTH `mcpp build` and `mcpp test` (the 0.0.97 double-path
#     invariant — activation must not diverge between the two).
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── leaf: a lib whose non-default feature `extra` carries a define ───────────
mkdir -p leaf/include/leaf leaf/src
cat > leaf/mcpp.toml <<'EOF'
[package]
name    = "leaf"
version = "0.1.0"

[features]
default = []
extra   = { defines = ["LEAF_EXTRA=1"] }

[build]
include_dirs = ["include"]

[targets.leaf]
kind = "lib"
EOF
cat > leaf/include/leaf/leaf.hpp <<'EOF'
#pragma once
inline int leaf_mode() {
#ifdef LEAF_EXTRA
    return 1;
#else
    return 0;
#endif
}
EOF
cat > leaf/src/leaf.cppm <<'EOF'
export module leaf;
export int leaf_anchor() { return 0; }
EOF

# ── mid: depends on leaf (default-features = false), forwards leaf/extra ─────
mkdir -p mid/src
cat > mid/mcpp.toml <<'EOF'
[package]
name    = "mid"
version = "0.1.0"

[features]
default   = []
withextra = ["leaf/extra"]

[dependencies]
leaf = { path = "../leaf", default-features = false }

[targets.mid]
kind = "lib"
EOF
cat > mid/src/mid.cppm <<'EOF'
export module mid;
export int mid_anchor() { return 0; }
EOF

# helper: does leaf's compile command carry the extra-feature define?
leaf_has_extra() {  # $1 = compile_commands.json path
    grep -q 'LEAF_EXTRA' "$1"
}

# ── Case A: app turns on mid/withextra → forward opens leaf/extra ────────────
mkdir -p appon/src
cat > appon/mcpp.toml <<'EOF'
[package]
name    = "appon"
version = "0.1.0"

[dependencies]
mid = { path = "../mid", features = ["withextra"] }
EOF
cat > appon/src/main.cpp <<'EOF'
int main() { return 0; }
EOF

( cd appon
  "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL(A): build failed"; exit 1; }
  leaf_has_extra compile_commands.json || {
      echo "FAIL(A): leaf/extra was NOT activated by the forward"; cat compile_commands.json; exit 1; }
  # double-path: the same forward must hold under `mcpp test`.
  "$MCPP" test > t.log 2>&1 || { cat t.log; echo "FAIL(A): test build failed"; exit 1; }
  leaf_has_extra compile_commands.json || {
      echo "FAIL(A/test): forward did not hold on the test path"; cat compile_commands.json; exit 1; }
)

# ── Case B: app does NOT turn on withextra → leaf/extra stays off ────────────
mkdir -p appoff/src
cat > appoff/mcpp.toml <<'EOF'
[package]
name    = "appoff"
version = "0.1.0"

[dependencies]
mid = { path = "../mid" }
EOF
cat > appoff/src/main.cpp <<'EOF'
int main() { return 0; }
EOF

( cd appoff
  "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL(B): build failed"; exit 1; }
  if leaf_has_extra compile_commands.json; then
      echo "FAIL(B): leaf/extra leaked though no consumer requested mid/withextra"
      cat compile_commands.json; exit 1
  fi
  "$MCPP" test > t.log 2>&1 || { cat t.log; echo "FAIL(B): test build failed"; exit 1; }
  if leaf_has_extra compile_commands.json; then
      echo "FAIL(B/test): leaf/extra leaked on the test path"; cat compile_commands.json; exit 1
  fi
)

echo "OK"
