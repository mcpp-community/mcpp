#!/usr/bin/env bash
# requires: gcc
# G5: mcpp.toml [features] `sources` key — feature-gated source globs, at
# parity with index descriptors. Four quadrants: feature off/on × build/test
# (the 0.0.94 lesson: feature-source behavior MUST match on both paths).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new featsrc > /dev/null
cd featsrc

cat > src/extra.cpp <<'EOF'
extern "C" int extra_answer() { return 42; }
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int extra_answer();
int main() {
#ifdef HAS_EXTRA
    std::println("extra = {}", extra_answer());
#else
    std::println("extra = none");
#endif
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "featsrc"
version = "0.1.0"

[features]
default = []
extra   = { sources = ["src/extra.cpp"], defines = ["HAS_EXTRA"] }
EOF

# Quadrant 1: feature OFF, build — extra.cpp must NOT compile (main has no
# reference so the link succeeds; the object must be absent).
"$MCPP" build > build_off.log 2>&1 || { cat build_off.log; echo "build (off) failed"; exit 1; }
find target -name 'extra.o' | grep -q . && { echo "extra.cpp compiled with feature off"; exit 1; } || true
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "extra = none" ]] || { echo "unexpected (off): $out"; exit 1; }

# Quadrant 2: feature ON, build (`run` has no --features; exec the binary).
"$MCPP" build --features extra > build_on.log 2>&1 || { cat build_on.log; echo "build (on) failed"; exit 1; }
# Feature defines change the fingerprint → a second output dir; take the
# newest binary.
bin="$(ls -t $(find target -name 'featsrc' -type f) | head -1)"
[[ -n "$bin" ]] || { echo "binary not found"; exit 1; }
out="$("./$bin" | tail -1)"
[[ "$out" == "extra = 42" ]] || { echo "unexpected (on): $out"; exit 1; }

# Quadrants 3+4: test path, feature off then on. The test binary references
# the gated symbol only when the feature is active.
mkdir -p tests
cat > tests/test_featsrc.cpp <<'EOF'
#ifdef HAS_EXTRA
extern "C" int extra_answer();
int main() { return extra_answer() == 42 ? 0 : 1; }
#else
int main() { return 0; }
#endif
EOF
"$MCPP" test > test_off.log 2>&1 || { cat test_off.log; echo "mcpp test (off) failed"; exit 1; }
"$MCPP" test --features extra > test_on.log 2>&1 || { cat test_on.log; echo "mcpp test (on) failed"; exit 1; }

echo "OK"
