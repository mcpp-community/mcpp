#!/usr/bin/env bash
# requires:
# 1066 — a version a merge replaces takes its requests out of the graph.
#
# The app asks for mcpplibs.os ^0.4.1, which resolves to the newest release
# (0.4.4, over the openkal 0.25 family); a path library pins os = 0.4.1, over
# the openkal 0.24 family. The merge takes os down to 0.4.1, and the requests
# 0.4.4 made -- openkal-linux 0.26.0, and through it openkal 0.25.0 -- leave
# with it (on Windows and macOS, the 0.25 family's implementation of that
# system). The library is named to be walked first, so that the pin is met
# before the newer release's own requests are. Before, they stayed: the graph
# was refused with "irreconcilable versions" between openkal 0.24.0 and a
# request of openkal-linux 0.26.0, a version no longer in it (the mtest
# workspace, measured).
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
TMP_HOST="$(host_path "$TMP")"
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/anchor/src" "$TMP/app/src"
cat > "$TMP/anchor/mcpp.toml" <<'EOF'
[package]
name    = "anchor"
version = "0.1.0"
[targets.anchor]
kind = "lib"

[dependencies]
mcpplibs.os = "0.4.1"
EOF
cat > "$TMP/anchor/src/anchor.cppm" <<'EOF'
export module anchor;
export int anchor_answer() { return 1; }
EOF

cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mcpplibs.os = "^0.4.1"
anchor = { path = "$TMP_HOST/anchor" }
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
import anchor;
int main() { return anchor_answer() == 1 ? 0 : 1; }
EOF

cd "$TMP/app"
"$MCPP" build --configure-only > plan.log 2>&1 || {
    cat plan.log; echo "FAIL: the merged graph was refused"; exit 1; }
if grep -q 'irreconcilable' plan.log; then
    cat plan.log; echo "FAIL: a request of a replaced version was kept"; exit 1
fi
grep -qE 'Merged mcpplibs\.os 0\.4\.[2-9] ⨯ 0\.4\.1 → v0\.4\.1' plan.log || {
    cat plan.log; echo "FAIL: os was not merged down to the pinned version"; exit 1; }
grep -A2 '^\[package\."openkal"\]' mcpp.lock | grep -q 'version = "0.24.0"' || {
    cat mcpp.lock; echo "FAIL: openkal is not the 0.24 the pinned os asks for"; exit 1; }
# The system's implementation is the one of the 0.24 family, whichever system
# this is: none of the 0.25 family's (linux 0.26, macos 0.23, windows 0.21).
for member in 'openkal-linux" 0.26' 'openkal-macos" 0.23' 'openkal-windows" 0.21'; do
    name=${member%% *}; series=${member##* }
    if grep -A2 "^\[package\.\"$name\]" mcpp.lock | grep -q "version = \"$series\."; then
        cat mcpp.lock; echo "FAIL: ${name%\"} $series of the replaced version is in the graph"; exit 1
    fi
done

echo "PASS: 1066 a merged version withdraws its requests"
