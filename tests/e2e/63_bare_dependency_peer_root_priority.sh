#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Bare selectors try mcpplibs first, then an independent peer-root package.
# This test provides only an independent root `imgui` package in the default
# index and verifies `imgui = "1.0.0"` does not resolve as mcpplibs.imgui.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$MCPP_HOME/registry/data/mcpplibs"
PKG_ROOT="$MCPP_HOME/registry/data/xpkgs/mcpplibs-x-imgui/1.0.0"
mkdir -p "$INDEX_DIR/pkgs/i" "$PKG_ROOT/src"
printf 'ok\n' > "$INDEX_DIR/.mcpp-index-updated"

cat > "$INDEX_DIR/pkgs/i/imgui.lua" <<'EOF'
package = {
    spec = "1",
    name = "imgui",
    description = "Independent bare selector fallback test package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/imgui-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/imgui.cppm" },
        targets = { ["imgui"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

cat > "$PKG_ROOT/src/imgui.cppm" <<'EOF'
export module imgui;

export int imgui_value() {
    return 42;
}
EOF
printf 'ok\n' > "$PKG_ROOT/.mcpp_ok"

mkdir -p "$TMP/project/app/src"
cd "$TMP/project/app"

cat > src/main.cpp <<'EOF'
import imgui;

int main() {
    return imgui_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
imgui = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    exit 1
}

grep -q '\[package."imgui"\]' mcpp.lock || {
    cat mcpp.lock
    echo "expected imgui package lock entry"
    exit 1
}

if grep -q 'namespace = "mcpplibs"' mcpp.lock; then
    cat mcpp.lock
    echo "bare independent package should not lock as mcpplibs"
    exit 1
fi

echo "OK"
