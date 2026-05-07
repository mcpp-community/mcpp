#!/usr/bin/env bash
# Verify dev-deps are NOT pulled by `mcpp build` but ARE pulled by `mcpp test`.
# We don't actually fetch a real dev-dep (would need network); we just verify
# that the dev-deps section in mcpp.toml does not appear in the build path's
# announcement output.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# Add a (path-based, doesn't actually exist) dev-dep — mcpp build should
# ignore it entirely; mcpp test would TRY to load it (and fail because path
# is fake). We don't actually run the test path; just verify build doesn't
# even mention it.
cat > mcpp.toml <<'EOF'
[package]
name        = "myapp"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.myapp]
kind = "bin"
main = "src/main.cpp"
[dev-dependencies."ghost-test-framework"]
path = "/nonexistent/ghost-package"
EOF

# `mcpp build` must succeed and NOT mention ghost-test-framework
out=$("$MCPP" build 2>&1)
echo "$out" | grep -q 'Finished' || { echo "build did not finish: $out"; exit 1; }
if echo "$out" | grep -q 'ghost-test-framework'; then
    echo "build path leaked dev-dep: $out"; exit 1
fi

# `mcpp test` should fail (because dev-dep path is bogus) — proving the
# dev-dep IS resolved in the test path.
rc=0
out=$("$MCPP" test 2>&1) || rc=$?
[[ $rc -ne 0 ]] || { echo "test should have failed (bogus dev-dep)"; exit 1; }
echo "$out" | grep -q 'ghost-test-framework' || {
    echo "test path didn't reference dev-dep: $out"; exit 1; }

echo "OK"
