#!/usr/bin/env bash
# requires: gcc
# mcpp#230: the source scanner follows directory symlinks (0.0.95), and the
# project-local `.mcpp/.xlings/data/<index>` entry is a SYMLINK back to the
# index root — following it walks that entire checkout. In CI that tree held
# a CJK-named file, whose wide→narrow spelling throws on MSVC (bare
# 0xC0000409 / exit 127). Assert both fixes:
#   1. `.mcpp` is pruned from glob walks — an out-of-tree file reachable only
#      through the symlink must NOT be compiled (it double-defines main).
#   2. A file whose name cannot be narrowed never aborts the build (covered
#      on MSVC by path_matches_glob's catch; here it just rides along).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p outside/sub proj/src

cat > outside/sub/extra.cpp <<'EOF'
int main() { return 9; }   // double-defines main if the walk escapes
EOF
# CJK filename — on MSVC hosts with a non-CJK ANSI codepage this name has no
# narrow spelling; the scanner must skip it, not tear down the build.
cat > "outside/sub/问题反馈.cpp" <<'EOF'
int cjk_never_compiled() { return 2; }
EOF

cat > proj/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > proj/mcpp.toml <<'EOF'
[package]
name = "prunetest"
version = "0.1.0"

[build]
sources = ["**/*.cpp"]
EOF

# Simulate the path-dep index link mcpp creates under the project data dir.
mkdir -p proj/.mcpp/.xlings/data
ln -s "$TMP/outside" proj/.mcpp/.xlings/data/compat

cd proj
"$MCPP" build
"$MCPP" run

echo "PASS: .mcpp symlink escape pruned; build unaffected by unmatchable names"
