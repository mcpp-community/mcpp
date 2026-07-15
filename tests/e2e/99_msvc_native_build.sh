#!/usr/bin/env bash
# requires: msvc
# 99_msvc_native_build.sh — native cl.exe builds (0.0.90):
#   - toolchain default msvc → new → build → run (import std via std.ixx/.ifc)
#   - a named module (.cppm through /interface /TP + /ifcOutput)
#   - incremental: touching one .cppm rebuilds, run still correct
set -e

CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
    # NB: match `default =` exactly — `default_target =` also starts with
    # "default" (the persisted pair since the naming unification).
    ORIG_DEFAULT=$(sed -n '/^\[toolchain\]/,/^\[/p' "$CONF" \
        | grep -E '^default[[:space:]]*=' | head -1 | cut -d'"' -f2 || true)
fi
TMP=$(mktemp -d)
restore() {
    if [[ -n "$ORIG_DEFAULT" ]]; then
        "$MCPP" toolchain default "$ORIG_DEFAULT" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap restore EXIT

cd "$TMP"
"$MCPP" toolchain default msvc >/dev/null \
    || { echo "FAIL: toolchain default msvc"; exit 1; }

"$MCPP" new hello_cl >/dev/null 2>&1
cd hello_cl
mkdir -p src
cat > src/greet.cppm <<'EOF'
export module hello.greet;
import std;
export namespace hello {
std::string greet() { return "cl-ok"; }
}
EOF
cat > src/main.cpp <<'EOF'
import std;
import hello.greet;
int main() { std::println("{}", hello::greet()); return 0; }
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: msvc build: $out"; exit 1; }
# the build must actually resolve msvc — a stale default_target could
# otherwise silently reroute to another toolchain (that exact bug shipped
# once: mingw's leftover default_target hijacked `toolchain default msvc`)
grep -q "msvc" <<<"$out" || { echo "FAIL: build did not resolve msvc: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: msvc run: $run_out"; exit 1; }
[[ "$run_out" == *"cl-ok"* ]] || { echo "FAIL: run output: $run_out"; exit 1; }

# .obj + .ifc artifacts really came from the msvc pipeline
find target -name "*.obj" | grep -q . || { echo "FAIL: no .obj produced"; exit 1; }
find target -path "*ifc.cache*" -name "*.ifc" | grep -q . \
    || { echo "FAIL: no .ifc produced"; exit 1; }

# incremental: edit the module, expect rebuild + updated output
sleep 1
sed -i 's/cl-ok/cl-ok2/' src/greet.cppm
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: incremental run: $run_out"; exit 1; }
[[ "$run_out" == *"cl-ok2"* ]] || { echo "FAIL: incremental output: $run_out"; exit 1; }

echo "PASS: native MSVC build — modules, import std, incremental"
