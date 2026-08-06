#!/usr/bin/env bash
# 195_target_cfg_feature_deps.sh — #359 D3a: `[target.<sel>.feature-deps.<f>]`.
#
# The conditional channel carried `dependencies`, `dev-dependencies` and
# `build-dependencies` and silently lacked `feature-deps`. That is the shape
# ConditionalConfig's own comment records for #258: the conditional reader
# keeps its own subset of the keys and falls behind without anyone noticing.
#
# It became load-bearing once a library could re-export build-time provisions.
# A library that puts a host tool behind a feature has no other way to say "not
# on this platform", and an unconditional declaration turns an unsupported
# platform into a hard error raised from inside the LIBRARY's manifest — which
# its user cannot edit and cannot work around.
#
# Two properties:
#   1. a matching predicate's feature-deps are pulled in when the feature is on
#   2. a non-matching predicate's are not — but the FEATURE still exists, so
#      requesting it on that platform is not an unknown-feature error
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

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
export int widget_anchor() { return 0; }
EOF

mkdir -p lib/src
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "mylib"
version = "0.1.0"

[build]
sources = ["src/mylib.cpp"]

# Exactly one of these matches on any of the three CI platforms, so the
# `codegen` feature always pulls widget in.
[target.'cfg(unix)'.feature-deps.codegen]
widget = { path = "../widget", visibility = "public" }
[target.'cfg(windows)'.feature-deps.codegen]
widget = { path = "../widget", visibility = "public" }

# Never matches. If a non-matching section were merged, resolving this bogus
# path would fail the build.
[target.'cfg(arch = "no_such_arch")'.feature-deps.codegen]
ghost = { path = "../this_path_does_not_exist" }
EOF
printf 'int mylib_fn(){return 1;}\n' > lib/src/mylib.cpp

# ── 1. feature on: the matching platform's feature-dep is resolved ──────────
mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mylib = { path = "../lib", features = ["codegen"] }
EOF
cat > app/src/main.cpp <<'EOF'
import widget;   // only reachable if the cfg-matched feature-dep resolved
int main() { return widget_anchor(); }
EOF
cd app
"$MCPP" build > b1.log 2>&1 || {
    cat b1.log
    echo "FAIL: a matching [target.*.feature-deps] was not merged"
    exit 1
}
if [ -f mcpp.lock ] && grep -q 'ghost' mcpp.lock; then
    echo "FAIL: a non-matching predicate's feature-dep was pulled in"; exit 1
fi

# ── 2. feature off: nothing is pulled in ───────────────────────────────────
cd "$TMP"
mkdir -p plain/src
cat > plain/mcpp.toml <<'EOF'
[package]
name    = "plain"
version = "0.1.0"

[dependencies]
mylib = { path = "../lib" }
EOF
printf 'int main(){}\n' > plain/src/main.cpp
cd plain
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: plain build failed"; exit 1; }
if [ -f mcpp.lock ] && grep -q 'widget' mcpp.lock; then
    echo "FAIL: an inactive feature's conditional dep was resolved anyway"; exit 1
fi

# ── 3. the feature exists on EVERY platform ────────────────────────────────
# Only what it pulls in is conditional. A consumer requesting it where no
# predicate matches must not be told the feature does not exist — that error
# would be the library's platform support leaking into its user's manifest.
cd "$TMP"
mkdir -p nomatch/src
cat > nomatch/mcpp.toml <<'EOF'
[package]
name    = "nomatchlib"
version = "0.1.0"

[build]
sources = ["src/nm.cpp"]

[target.'cfg(arch = "no_such_arch")'.feature-deps.codegen]
ghost = { path = "../this_path_does_not_exist" }
EOF
printf 'int nm(){return 1;}\n' > nomatch/src/nm.cpp
mkdir -p nmapp/src
cat > nmapp/mcpp.toml <<'EOF'
[package]
name    = "nmapp"
version = "0.1.0"

[dependencies]
nomatchlib = { path = "../nomatch", features = ["codegen"] }
EOF
printf 'int main(){}\n' > nmapp/src/main.cpp
cd nmapp
# Asserted on the DIAGNOSTIC rather than with --strict: --strict promotes every
# degradation, including unrelated ones (clang on Windows reports "this
# toolchain and platform combination emits no GNU depfile"), so it would make
# this test fail for a reason it is not about.
"$MCPP" build > b3.log 2>&1 || {
    cat b3.log
    echo "FAIL: requesting a conditionally-populated feature failed where no predicate matches"
    exit 1
}
grep -q "does not declare requested feature" b3.log && {
    cat b3.log
    echo "FAIL: the feature was reported as undeclared where no predicate matches"
    exit 1
}

echo "PASS: 195_target_cfg_feature_deps"
