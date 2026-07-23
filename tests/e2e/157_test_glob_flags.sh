#!/usr/bin/env bash
# requires:
# [build].flags globs also cover tests/ TUs (via the per-target flag channel),
# and a glob that matches real files on disk (just not scanned sources) does
# NOT warn "matched no source file".
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "globtest"
version = "0.1.0"
standard = "c++23"

[build]
flags = [
  { glob = "tests/tagged*.cpp", cxxflags = ["-DTAG=7"] },
  { glob = "tests/never/**",    cxxflags = ["-DNOPE"] },
]
EOF
cat > tests/tagged.cpp <<'EOF'
#ifndef TAG
#error "per-glob cxxflags did not reach the test TU"
#endif
int main() { return TAG == 7 ? 0 : 1; }
EOF
cat > tests/plain.cpp <<'EOF'
#ifdef TAG
#error "per-glob cxxflags leaked into an unmatched test TU"
#endif
int main() { return 0; }
EOF

out=$("$MCPP" test 2>&1) || { echo "mcpp test failed: $out"; exit 1; }
[[ "$out" == *"tagged ... ok"* ]] || { echo "tagged test failed: $out"; exit 1; }
[[ "$out" == *"plain ... ok"* ]]  || { echo "plain test failed: $out"; exit 1; }
[[ "$out" != *"'tests/tagged*.cpp' matched no source file"* ]] \
    || { echo "spurious dead-glob warning for a tests/ glob: $out"; exit 1; }
[[ "$out" == *"'tests/never/**' matched no source file"* ]] \
    || { echo "genuinely dead glob must still warn: $out"; exit 1; }
echo OK
