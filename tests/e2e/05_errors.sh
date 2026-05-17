#!/usr/bin/env bash
# Error paths: missing manifest, missing version, conditional import, header unit, naming violation
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# 1. No manifest
cd "$TMP"
mkdir -p empty
cd empty
out=$("$MCPP" build 2>&1) && { echo "expected failure but build succeeded"; exit 1; }
[[ "$out" == *"no mcpp.toml"* ]] || { echo "wrong error: $out"; exit 1; }

# 2. Missing version
cd "$TMP"
mkdir -p bad-manifest
cd bad-manifest
cat > mcpp.toml <<'EOF'
[package]
name = "bad"
[language]
[modules]
sources = ["src/**/*.cppm"]
[targets.bad]
kind = "bin"
main = "src/main.cpp"
EOF
out=$("$MCPP" build 2>&1) && { echo "expected failure"; exit 1; }
[[ "$out" == *"package.version"* ]] || { echo "wrong error: $out"; exit 1; }

# 3. Conditional import
cd "$TMP"
"$MCPP" new bad-cond > /dev/null
cd bad-cond
cat > src/main.cpp <<'EOF'
import std;
#ifdef WANT_FOO
import foo;
#endif
int main() {}
EOF
out=$("$MCPP" build 2>&1) && { echo "expected failure"; exit 1; }
[[ "$out" == *"conditional preprocessor"* ]] || { echo "wrong error: $out"; exit 1; }

# 4. Header unit
cd "$TMP"
"$MCPP" new bad-header > /dev/null
cd bad-header
cat > src/main.cpp <<'EOF'
import std;
import "header.h";
int main() {}
EOF
out=$("$MCPP" build 2>&1) && { echo "expected failure"; exit 1; }
[[ "$out" == *"header units"* ]] || { echo "wrong error: $out"; exit 1; }

# 5. Module naming is the library author's choice (0.0.10+).
# No prefix enforcement — this test just verifies we REMOVED the check.
cd "$TMP"
"$MCPP" new naming-ok > /dev/null
cd naming-ok
sed -i.bak 's/name        = "naming-ok"/name        = "myorg.something"/' mcpp.toml && rm -f mcpp.toml.bak
cat > src/foo.cppm <<'EOF'
export module differentprefix;
import std;
EOF
# This should succeed now (no naming violation error).
"$MCPP" build > /dev/null 2>&1 || { echo "expected success but build failed"; exit 1; }

echo "OK"
