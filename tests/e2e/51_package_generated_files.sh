#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Form B package descriptors can materialize small package-owned support
# files before scanning and compiling the package.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/t"
cat > "$INDEX_DIR/pkgs/t/tinycfg.lua" <<'EOF'
package = {
    spec = "1",
    name = "tinycfg",
    description = "Generated config header package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/tinycfg-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/*.c" },
        include_dirs = { "mcpp_generated/include" },
        generated_files = {
            ["mcpp_generated/include/generated_config.h"] = "#pragma once\n#define TINYCFG_VALUE 42\n",
        },
        targets = { ["tinycfg"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

mkdir -p "$TMP/project/app/src" \
         "$TMP/project/app/.mcpp/.xlings/data/xpkgs/local-dev.tinycfg/1.0.0/src"
cd "$TMP/project/app"

cat > .mcpp/.xlings/data/xpkgs/local-dev.tinycfg/1.0.0/src/tinycfg.c <<'EOF'
#include "generated_config.h"
int tinycfg_value(void) {
    return TINYCFG_VALUE;
}
EOF

cat > src/main.cpp <<'EOF'
extern "C" int tinycfg_value(void);
int main() {
    return tinycfg_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR" }

[dependencies]
"local-dev.tinycfg" = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    exit 1
}

generated=".mcpp/.xlings/data/xpkgs/local-dev.tinycfg/1.0.0/mcpp_generated/include/generated_config.h"
[[ -f "$generated" ]] || {
    echo "FAIL: generated file was not materialized"
    find .mcpp/.xlings/data/xpkgs/local-dev.tinycfg/1.0.0 -maxdepth 4 -type f | sort
    exit 1
}

grep -q "TINYCFG_VALUE 42" "$generated" || {
    echo "FAIL: generated file content mismatch"
    cat "$generated"
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    exit 1
}

echo "OK"
