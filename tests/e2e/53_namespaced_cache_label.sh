#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Cached dependency status must use the canonical dependency key, not only the
# short package name embedded in the dependency's own mcpp.toml.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/c"
cat > "$INDEX_DIR/pkgs/c/compat.widget.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "compat",
    name = "compat.widget",
    description = "Namespaced cache label package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/widget-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
EOF

mkdir -p "$TMP/project/app/src" \
         "$TMP/project/app/.mcpp/.xlings/data/xpkgs/compat.widget/1.0.0/widget-1.0.0/src"
cd "$TMP/project/app"

cat > .mcpp/.xlings/data/xpkgs/compat.widget/1.0.0/widget-1.0.0/mcpp.toml <<'EOF'
[package]
namespace = "compat"
name = "widget"
version = "1.0.0"

[build]
sources = ["src/*.c"]

[targets.widget]
kind = "lib"
EOF

cat > .mcpp/.xlings/data/xpkgs/compat.widget/1.0.0/widget-1.0.0/src/widget.c <<'EOF'
int widget_value(void) {
    return 7;
}
EOF

cat > src/main.cpp <<'EOF'
extern "C" int widget_value(void);
int main() {
    return widget_value() == 7 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
compat = { path = "$INDEX_DIR" }

[dependencies.compat]
widget = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    exit 1
}

if grep -Eq '^\[[0-9]+/[0-9]+\] ' build.log; then
    echo "FAIL: automatic index refresh should not expose xlings update output"
    cat build.log
    exit 1
fi

[[ -f "$MCPP_HOME/registry/data/mcpplibs/.mcpp-index-updated" ]] || {
    echo "FAIL: automatic index refresh should write mcpp freshness marker"
    find "$MCPP_HOME/registry/data" -maxdepth 3 -type f | sort
    cat build.log
    exit 1
}

rm -rf target
"$MCPP" build > build2.log 2>&1 || {
    cat build2.log
    exit 1
}

grep -q "Cached compat.widget v1.0.0" build2.log || {
    echo "FAIL: cached namespaced dependency should be reported as compat.widget"
    cat build2.log
    exit 1
}

echo "OK"
