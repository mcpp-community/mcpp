#!/usr/bin/env bash
# 21_ninja_dyndep.sh — verify M3.3.b: ninja dyndep build mode produces
# byte-identical runtime output to the static-deps mode.
#
# Toggled by env: MCPP_NINJA_DYNDEP=1
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new myapp > /dev/null
cd myapp

# Multi-module project to exercise scan + collect + dyndep BMI edges.
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
int main() { banner(); greet("dyndep"); return 0; }
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

# --- Path A: static deps (default) ---
"$MCPP" build > build.static.log 2>&1
triple=$(ls -d target/x86_64-linux-*/ | head -1)
fp_dir=$(ls "$triple")
out_static=$(${triple}${fp_dir}/bin/myapp)
rm -rf target

# --- Path B: dyndep mode ---
MCPP_NINJA_DYNDEP=1 "$MCPP" build > build.dyndep.log 2>&1
triple=$(ls -d target/x86_64-linux-*/ | head -1)
fp_dir=$(ls "$triple")
out_dyndep=$(${triple}${fp_dir}/bin/myapp)

# --- Assertions ---
if [[ "$out_static" != "$out_dyndep" ]]; then
    echo "FAIL: runtime output differs"
    echo "  static: $out_static"
    echo "  dyndep: $out_dyndep"
    exit 1
fi

# Dyndep mode must have created the dyndep file.
[[ -f "${triple}${fp_dir}/build.ninja.dd" ]] || {
    echo "FAIL: build.ninja.dd not produced under MCPP_NINJA_DYNDEP=1"
    exit 1
}

# Dyndep mode must have emitted scan rules.
grep -q '^rule cxx_scan'    ${triple}${fp_dir}/build.ninja || {
    echo "FAIL: build.ninja missing cxx_scan rule"; exit 1; }
grep -q '^rule cxx_collect' ${triple}${fp_dir}/build.ninja || {
    echo "FAIL: build.ninja missing cxx_collect rule"; exit 1; }
grep -q '  dyndep = build.ninja.dd' ${triple}${fp_dir}/build.ninja || {
    echo "FAIL: compile edges missing dyndep ="; exit 1; }

# Static mode must NOT have those rules (sanity).
grep -q '^rule cxx_scan' build.static.log && {
    echo "FAIL: static mode leaked cxx_scan"; exit 1; } || true

# Verify .ddi file content for one TU is real P1689 JSON.
ddi=$(find target -name '*.cppm.ddi' | head -1)
[[ -n "$ddi" ]] || { echo "FAIL: no .ddi file produced"; exit 1; }
grep -q '"rules"' "$ddi"          || { echo "FAIL: .ddi missing rules"; exit 1; }
grep -q '"primary-output"' "$ddi" || { echo "FAIL: .ddi missing primary-output"; exit 1; }

# build.ninja.dd content sanity.
ddep="${triple}${fp_dir}/build.ninja.dd"
grep -q 'ninja_dyndep_version = 1' "$ddep" || {
    echo "FAIL: dyndep file missing version header"; exit 1; }
grep -q 'gcm.cache/myapp.lib-greet.gcm' "$ddep" || {
    echo "FAIL: dyndep file missing partition BMI"; cat "$ddep"; exit 1; }

# Incremental: re-run dyndep build → must be noop.
out2=$(MCPP_NINJA_DYNDEP=1 "$MCPP" build 2>&1)
[[ "$out2" == *'Finished'* ]] || { echo "FAIL: incremental rebuild output: $out2"; exit 1; }

echo "OK"
