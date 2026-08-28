#!/usr/bin/env bash
# requires: elf
# 308_dependency_required_features.sh — a DEPENDENCY's target gate (issue #519).
#
# `[targets.<n>] required_features` was filtered for the root package only.
# A dependency that wrote it got the OPPOSITE of what it asked for: the target
# was emitted for every consumer, active feature or not.
#
# That is not cosmetic when the gated target is `kind = "shared"`. A package
# with any shared target has ALL of its objects taken out of every consumer's
# link and put behind a shared library instead — so an "optional" target
# silently changed how the package was linked into everyone.
#
# The criterion is the ARTIFACT, not a log line: with the feature off there
# must be no shared object; with it on there must be one.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p dep/src app/src

cat > dep/mcpp.toml <<'EOF'
[package]
name    = "dep"
version = "0.1.0"
[build]
c_standard = "c11"
sources    = ["src/*.c"]
[features]
plugin = []
[targets.dep]
kind = "lib"
[targets.depshared]
kind = "shared"
required_features = ["plugin"]
EOF
cat > dep/src/dep.c <<'EOF'
int dep_value(void) { return 5; }
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[build]
c_standard = "c11"
[targets.app]
kind = "bin"
main = "src/main.c"
[dependencies]
dep = { path = "../dep" }
EOF
cat > app/src/main.c <<'EOF'
#include <stdio.h>
int dep_value(void);
int main(void) { printf("%d\n", dep_value()); return 0; }
EOF

cd app

# ── the gate is CLOSED: no shared object, and the program still works ───────
"$MCPP" build > off.log 2>&1 || { cat off.log; exit 1; }
if find target -name 'libdepshared.so' | grep -q .; then
    echo "FAIL: a feature-gated dependency target was built with the feature off"
    find target -name 'libdepshared.so'
    exit 1
fi
bin="$(find target -name app -type f | head -1)"
[[ -n "$bin" ]] || { cat off.log; echo "FAIL: no executable"; exit 1; }
"$bin" | grep -qx 5 || { echo "FAIL: the gated-off build does not run"; exit 1; }

# ── the gate is OPEN: the target appears ───────────────────────────────────
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[build]
c_standard = "c11"
[targets.app]
kind = "bin"
main = "src/main.c"
[dependencies]
dep = { path = "../dep", features = ["plugin"] }
EOF

"$MCPP" build > on.log 2>&1 || { cat on.log; exit 1; }
so="$(find target -name 'libdepshared.so' -type f | head -1)"
[[ -n "$so" ]] || {
    echo "FAIL: requesting the feature did not build the gated target"
    cat on.log; exit 1; }

echo "ok: a dependency's required_features gate opens and closes"
