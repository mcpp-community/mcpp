#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Regression test for dependency BMI cache population when a custom-index
# package produces object files under collision-avoidance subdirectories.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/c"
cat > "$INDEX_DIR/pkgs/c/collision-lib.lua" <<'EOF'
package = {
    spec = "1",
    name = "collision-lib",
    description = "Collision object path package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/collision-lib-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["collision-lib"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

mkdir -p "$TMP/project/app"
cd "$TMP/project/app"

mkdir -p src .mcpp/.xlings/data/xpkgs/local-dev.collision-lib/1.0.0/src/a \
         .mcpp/.xlings/data/xpkgs/local-dev.collision-lib/1.0.0/src/b

cat > src/main.cpp <<'EOF'
import std;
import collision.a;
import collision.b;
int main() {
    std::println("{} {}", collision_a(), collision_b());
    return 0;
}
EOF

cat > .mcpp/.xlings/data/xpkgs/local-dev.collision-lib/1.0.0/src/a/foo.cppm <<'EOF'
export module collision.a;
export int collision_a() { return 1; }
EOF

cat > .mcpp/.xlings/data/xpkgs/local-dev.collision-lib/1.0.0/src/b/foo.cppm <<'EOF'
export module collision.b;
export int collision_b() { return 2; }
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR" }

[dependencies]
"local-dev.collision-lib" = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

if grep -q "Fetching custom index repos" build.log; then
    echo "FAIL: local path index should not trigger xlings update noise"
    cat build.log
    exit 1
fi

if grep -q "bmi cache populate failed" build.log; then
    echo "FAIL: BMI cache populate warning should not appear"
    cat build.log
    exit 1
fi

entry="$(find "$MCPP_HOME/build-cache/v1/pkg/local-dev" -name entry.json | head -1)"
[[ -n "$entry" ]] || {
    echo "FAIL: missing custom-index cache entry"
    find "$MCPP_HOME/build-cache/v1" -maxdepth 6 -type f | sort
    cat build.log
    exit 1
}

# Read the structured entry instead of grepping a log: entry.json exists so the
# cache describes itself, and every object it lists must be on disk under obj/.
python3 - "$entry" > entrycheck.log 2>&1 <<'PYEOF' || { cat entrycheck.log; exit 1; }
import json, os, sys
entry = sys.argv[1]
d = json.load(open(entry))
objs = d.get("obj", [])
if not [o for o in objs if "/" in o and o.endswith("foo.m.o")]:
    sys.exit(f"FAIL: entry did not preserve a nested object path: {objs}")
root = os.path.dirname(entry)
for o in objs:
    p = os.path.join(root, "obj", o)
    if not os.path.exists(p):
        sys.exit(f"FAIL: listed object missing on disk: {p}")
PYEOF

rm -rf target
"$MCPP" build > build2.log 2>&1 || { cat build2.log; exit 1; }

grep -q "Cached local-dev.collision-lib v1.0.0" build2.log || {
    echo "FAIL: second cold build did not reuse the build cache"
    cat build2.log
    exit 1
}

# A reused dependency must not be recompiled — that was the whole defect. Its
# outputs are declared by stage_file edges now, so there is no compile edge for
# its sources at all.
NINJA="$(find target -name build.ninja | head -1)"
[[ -n "$NINJA" ]] || { echo "FAIL: no build.ninja"; exit 1; }
if grep -qE ': (cxx_module|cxx_object) .*collision-lib' "$NINJA"; then
    echo "FAIL: cached dependency still has compile edges"
    grep -nE ': (cxx_module|cxx_object) .*collision-lib' "$NINJA"
    exit 1
fi
grep -qE '^build .* : stage_file .*collision-lib' "$NINJA" || {
    echo "FAIL: cached dependency has no stage_file edge"
    grep -n 'stage_file' "$NINJA" | head
    exit 1
}

echo "OK"
