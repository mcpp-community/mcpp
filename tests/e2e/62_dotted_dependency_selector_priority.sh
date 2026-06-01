#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Dotted selectors in a single [dependencies] table use ordered candidates:
#   imgui.core -> mcpplibs.imgui/core, then imgui/core.
# This test provides only the peer-root imgui/core package and verifies the
# build resolves through that fallback without network access.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/imgui-index"
mkdir -p "$INDEX_DIR/pkgs/i"
cat > "$INDEX_DIR/pkgs/i/imgui.core.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "imgui",
    name = "imgui.core",
    description = "Dotted selector fallback test package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/imgui-core-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/core.cppm" },
        targets = { ["core"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

mkdir -p "$TMP/project/app/src" \
         "$TMP/project/app/.mcpp/.xlings/data/xpkgs/imgui-x-imgui.core/1.0.0/src"
cd "$TMP/project/app"

cat > .mcpp/.xlings/data/xpkgs/imgui-x-imgui.core/1.0.0/src/core.cppm <<'EOF'
export module imgui.core;

export int imgui_core_value() {
    return 42;
}
EOF

cat > src/main.cpp <<'EOF'
import imgui.core;

int main() {
    return imgui_core_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
imgui = { path = "$INDEX_DIR" }

[dependencies]
imgui.core = "1.0.0"

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

grep -q 'namespace = "imgui"' mcpp.lock || {
    cat mcpp.lock
    echo "expected lockfile to record resolved peer namespace imgui"
    exit 1
}

echo "OK"
