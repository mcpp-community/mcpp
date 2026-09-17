#!/usr/bin/env bash
# 739 -- a platform SDK pulled under [feature-deps.<feature>] with
# `visibility = "private"` reaches only the declaring package's own
# translation units, never a consumer's (mcpp#662, M5).
#
# WHY THIS TEST EXISTS. #662's plan required VERIFYING, not assuming, that a
# platform-bound package can keep a platform dependency to itself before
# documenting the pattern (docs/06, docs/24). Reading `computeUsageRequirements`
# in prepare.cppm shows a `DependencyVisibility` axis already exists and that
# `Private` folds a dependency's usage into `consumer.privateBuild` only, never
# into `consumer.publicUsage` — so it should already hold for ANY dependency
# table, `[feature-deps.<name>]` included, since both route through the same
# per-entry parser. This test is the measurement that confirms it, and it
# fails on any future change that lets a private edge's headers leak.
#
# THE SHAPE, deliberately proving both directions on the SAME two packages so
# a change to the harness itself cannot make this pass vacuously:
#
#   sdk (a path package)      -- a header defining SDK_PRESENT
#   lib_platform (a path pkg) -- [feature-deps.plat] depends on `sdk`
#   app (this test's root)    -- depends on lib_platform with features=["plat"]
#
#   A. `sdk` declared PRIVATE  -> lib_platform's own TU sees it (must build);
#                                 app's TU including "sdk.h" must NOT build.
#   B. `sdk` declared PUBLIC   -> app's TU including "sdk.h" DOES build --
#                                 establishing that the harness would have
#                                 caught A being wrong, not merely different.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

mkdir -p "$TMP/sdk/src" "$TMP/sdk/include"
cat > "$TMP/sdk/mcpp.toml" <<'EOF'
[package]
name    = "sdk"
version = "0.1.0"

[targets.sdk]
kind = "lib"
EOF
cat > "$TMP/sdk/include/sdk.h" <<'EOF'
#pragma once
#define SDK_PRESENT 1
inline int sdk_value(void) { return 42; }
EOF
cat > "$TMP/sdk/src/empty.cpp" <<'EOF'
// no compiled sources of its own beyond the header
EOF

mkdir -p "$TMP/lib_platform/src"
write_lib_platform_manifest() {   # $1 = visibility
    cat > "$TMP/lib_platform/mcpp.toml" <<EOF
[package]
name    = "lib_platform"
version = "0.1.0"

[targets.lib_platform]
kind = "lib"

[features]
plat = {}

[feature-deps.plat]
sdk = { path = "../sdk", visibility = "$1" }
EOF
}
cat > "$TMP/lib_platform/src/lib_platform.cpp" <<'EOF'
#include "sdk.h"
#ifndef SDK_PRESENT
#error "lib_platform's own translation unit must see sdk's headers"
#endif
int lib_platform_uses_sdk(void) { return sdk_value(); }
EOF

mkdir -p "$TMP/app/src"
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
lib_platform = { path = "../lib_platform", features = ["plat"] }
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
#include "sdk.h"
int main() { return 0; }
EOF

# A. private — lib_platform's own build must still succeed (it needs sdk.h),
#    and app's must fail to find sdk.h at all: not merely "SDK_PRESENT
#    undefined" (which private defines/cflags would also produce) but the
#    header itself absent from app's search path.
write_lib_platform_manifest private
cd "$TMP/app"
if out="$("$MCPP" build --cache off 2>&1)"; then
    printf '%s\n' "$out" > "$TMP/a-build.log"
    fail "A: app built despite depending (via a private feature-dep) on a package whose headers it must not see" "$TMP/a-build.log"
fi
printf '%s\n' "$out" > "$TMP/a-build.log"
case "$out" in
    *"sdk.h"*"No such file"*|*"sdk.h"*"not found"*|*"sdk.h"*"cannot open"*)
        echo "  ok  A: app cannot find sdk.h (private feature-dep did not cross the boundary)" ;;
    *)
        fail "A: app's build failed, but not on a missing sdk.h — cannot tell privacy from an unrelated break" "$TMP/a-build.log" ;;
esac

# lib_platform's OWN build (its private dependency reaches its own TUs).
# `--features plat`: `plat` is not a default feature, and building
# lib_platform AS THE ROOT package activates only `default` unless told
# otherwise — the same rule app's own dependency edge states explicitly.
cd "$TMP/lib_platform"
"$MCPP" build --features plat --cache off > "$TMP/lp-build.log" 2>&1 \
    || fail "A: lib_platform (the declaring package) could not build against its own private feature-dep" "$TMP/lp-build.log"
echo "  ok  A: lib_platform's own translation unit does see sdk's headers"

# B. public — the same app now builds, proving the assertion above would
#    have failed had the engine actually broadcast a private edge.
write_lib_platform_manifest public
cd "$TMP/app"
"$MCPP" build --cache off > "$TMP/b-build.log" 2>&1 \
    || fail "B: app failed to build once sdk is a PUBLIC feature-dep of lib_platform — the harness cannot distinguish private from broken" "$TMP/b-build.log"
echo "  ok  B: with visibility=public the same app DOES see sdk.h (the harness is not vacuous)"

echo "PASS: 739 a private feature-dep does not reach the consumer"
