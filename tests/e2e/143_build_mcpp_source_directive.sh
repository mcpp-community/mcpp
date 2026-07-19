#!/usr/bin/env bash
# requires: gcc
# P1 (large-source-pkg design §3.1): `mcpp:source=` selects a PRE-EXISTING
# source file into the compile set — the proper name for the old `generated=`
# absolute-path grey usage. The file lives in the package tree, is NOT matched
# by any [build]/[modules] sources glob, and the program did not write it.
# Also covers: the typed `mcpp::source(...)` emitter, and the `d source` cache
# record round-trip (a rebuild that replays the cache must still compile it).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p srcsel/src srcsel/extra
cd srcsel

# Pre-existing payload file OUTSIDE the sources globs (extra/, not src/).
cat > extra/impl.cpp <<'EOF'
extern "C" int selected_value() { return 42; }
EOF

# build.mcpp selects it via the typed API (relative to the package root).
cat > build.mcpp <<'EOF'
import mcpp;
int main() {
    mcpp::source("extra/impl.cpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int selected_value();
int main() {
    std::println("SELECTED={}", selected_value());
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "srcsel"
version = "0.1.0"

[modules]
sources = ["src/**/*.cpp"]

[targets.srcsel]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build1.log 2>&1 || { cat build1.log; echo "FAIL: build failed"; exit 1; }
grep -q "ignoring unknown directive" build1.log && {
    cat build1.log; echo "FAIL: mcpp:source= not recognized"; exit 1; } || true

out="$("$MCPP" run 2>&1 | grep '^SELECTED=' | tail -1)"
[[ "$out" == "SELECTED=42" ]] || { echo "FAIL: selected source not linked: $out"; exit 1; }

# Cache round-trip: touch main.cpp (invalidates the ninja build, not the
# build.mcpp inputs) → the cached `d source` record must reapply so the
# selected file stays in the compile set without a re-run.
cat > src/main.cpp <<'EOF'
import std;
extern "C" int selected_value();
int main() {
    std::println("SELECTED2={}", selected_value());
    return 0;
}
EOF
"$MCPP" build > build2.log 2>&1 || { cat build2.log; echo "FAIL: rebuild failed"; exit 1; }
grep -q "build.mcpp running" build2.log && {
    cat build2.log; echo "FAIL: unchanged build.mcpp re-ran"; exit 1; } || true
out="$("$MCPP" run 2>&1 | grep '^SELECTED2=' | tail -1)"
[[ "$out" == "SELECTED2=42" ]] || { echo "FAIL: cached source record lost: $out"; exit 1; }

# A source= pointing at a missing file is a hard, actionable error.
cat > build.mcpp <<'EOF'
import mcpp;
int main() {
    mcpp::source("extra/nope.cpp");
    return 0;
}
EOF
"$MCPP" build > build3.log 2>&1 && { cat build3.log; echo "FAIL: missing source did not fail"; exit 1; }
grep -q "selected source" build3.log || {
    cat build3.log; echo "FAIL: missing-source error not surfaced"; exit 1; }

echo "OK"
