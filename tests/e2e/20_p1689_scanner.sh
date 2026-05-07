#!/usr/bin/env bash
# 20_p1689_scanner.sh — verify M3.3.a opt-in P1689 scanner end-to-end.
#
# Builds the same multi-module project twice — once under the default
# regex scanner, once under MCPP_SCANNER=p1689 — and verifies:
#   1. Both invocations succeed.
#   2. The runtime output of the produced binary is identical.
#   3. The compile graph (set of compiled .gcm files) matches.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new myapp > /dev/null
cd myapp

# A small project with a partition + a consumer to exercise scanner edges.
cat > src/lib.cppm <<'EOF'
export module myapp.lib;
import std;
export import :greet;
export auto banner() -> void { std::println("== myapp =="); }
EOF

cat > src/lib-greet.cppm <<'EOF'
export module myapp.lib:greet;
import std;
export auto greet(std::string_view who) -> void {
    std::println("hello, {}", who);
}
EOF

cat > src/main.cpp <<'EOF'
import std;
import myapp.lib;
int main() {
    banner();
    greet("p1689");
    return 0;
}
EOF

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
EOF

# --- Path A: regex scanner (default) ---
"$MCPP" build > build.regex.log 2>&1
triple=$(ls -d target/x86_64-linux-*/ | head -1)
fp_dir=$(ls "$triple")
out_regex=$(${triple}${fp_dir}/bin/myapp)
gcm_regex=$(find target -name '*.gcm' | sort | xargs -n1 basename | sort -u)
rm -rf target

# --- Path B: P1689 scanner ---
MCPP_SCANNER=p1689 "$MCPP" build > build.p1689.log 2>&1
triple=$(ls -d target/x86_64-linux-*/ | head -1)
fp_dir=$(ls "$triple")
out_p1689=$(${triple}${fp_dir}/bin/myapp)
gcm_p1689=$(find target -name '*.gcm' | sort | xargs -n1 basename | sort -u)

# --- Compare ---
if [[ "$out_regex" != "$out_p1689" ]]; then
    echo "FAIL: runtime output differs"
    echo "  regex: $out_regex"
    echo "  p1689: $out_p1689"
    exit 1
fi

if [[ "$gcm_regex" != "$gcm_p1689" ]]; then
    echo "FAIL: compiled BMI set differs"
    echo "--- regex ---"; echo "$gcm_regex"
    echo "--- p1689 ---"; echo "$gcm_p1689"
    exit 1
fi

# Sanity: the expected modules really were compiled.
echo "$gcm_p1689" | grep -q 'myapp.lib.gcm'        || { echo "missing myapp.lib.gcm"; exit 1; }
echo "$gcm_p1689" | grep -q 'myapp.lib-greet.gcm'  || { echo "missing partition gcm"; exit 1; }

echo "OK"
