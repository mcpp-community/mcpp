#!/usr/bin/env bash
# requires: gcc
# `mcpp test` honors --profile / --features (issue #131 scenario B). The flag
# selects a whole-build mode for the test build, so it reaches the CODE-UNDER-
# TEST (a shared src/ unit), not just the test entry. Plain `mcpp test` must
# fail; `mcpp test --profile observe` must pass.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p proj/src proj/tests
cd proj

# Code under test — a shared library source, compiled once and linked into the
# test binary. Its behavior depends on a macro supplied only by the profile.
cat > src/probe.cpp <<'EOF'
extern "C" int probe_mode(void) {
#ifdef OBSERVE_MODE
    return 1;
#else
    return 0;
#endif
}
EOF

cat > tests/test_mode.cpp <<'EOF'
extern "C" int probe_mode(void);
int main() { return probe_mode() == 1 ? 0 : 1; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "proj"
version = "0.1.0"

[build]
sources = ["src/**/*.cpp"]

[profile.observe]
cxxflags = ["-DOBSERVE_MODE=1"]
EOF

# Default profile: the code-under-test is built WITHOUT the macro → test fails.
if "$MCPP" test > plain.log 2>&1; then
    cat plain.log
    echo "expected default 'mcpp test' to fail (macro should be absent)"
    exit 1
fi

# --profile observe: the macro reaches src/probe.cpp → test passes.
"$MCPP" test --profile observe > observe.log 2>&1 || {
    cat observe.log
    echo "expected 'mcpp test --profile observe' to pass"
    exit 1
}

echo "OK"
