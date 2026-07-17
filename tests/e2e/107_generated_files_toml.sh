#!/usr/bin/env bash
# requires: gcc
# G6: mcpp.toml [generated_files] — path = "contents" entries materialize
# into the project root before the scan (multiline TOML strings), enter the
# fingerprint (content change → rebuild), and escaping paths hard-error.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new genf > /dev/null
cd genf
rm -f src/main.cpp

cat > mcpp.toml <<'EOF'
[package]
name    = "genf"
version = "0.1.0"

[generated_files]
"src/gen/answer.cpp" = """
extern "C" int gen_answer() { return 41; }
"""
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int gen_answer();
int main() {
    std::println("gen = {}", gen_answer());
    return 0;
}
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
[[ -f src/gen/answer.cpp ]] || { echo "generated file not materialized"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "gen = 41" ]] || { echo "unexpected output: $out"; exit 1; }

# Content change re-materializes and rebuilds (content is in the fingerprint).
sed -i.bak 's/return 41/return 42/' mcpp.toml
"$MCPP" build > rebuild.log 2>&1 || { cat rebuild.log; echo "rebuild failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "gen = 42" ]] || { echo "stale after content change: $out"; exit 1; }

# Escaping path must be rejected at parse time.
cat > mcpp.toml <<'EOF'
[package]
name    = "genf"
version = "0.1.0"
[generated_files]
"../evil.cpp" = "int x;"
EOF
"$MCPP" build > escape.log 2>&1 && { echo "escaping path was accepted"; exit 1; }
grep -qi "generated_files" escape.log || { cat escape.log; echo "wrong error"; exit 1; }

echo "OK"
