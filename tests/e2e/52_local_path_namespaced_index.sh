#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Local path indices must use the same namespace-aware filename candidates
# as cloned/builtin indices, e.g. pkgs/c/compat.foo.lua.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/c"
cat > "$INDEX_DIR/pkgs/c/compat.cfg.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "compat",
    name = "compat.cfg",
    description = "Namespaced local path index package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/cfg-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/*.c" },
        targets = { ["cfg"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

mkdir -p "$TMP/project/app/src" \
         "$TMP/project/app/.mcpp/.xlings/data/xpkgs/compat.cfg/1.0.0/src"
cd "$TMP/project/app"

cat > .mcpp/.xlings/data/xpkgs/compat.cfg/1.0.0/src/cfg.c <<'EOF'
int cfg_value(void) {
    return 42;
}
EOF

cat > src/main.cpp <<'EOF'
extern "C" int cfg_value(void);
int main() {
    return cfg_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
compat = { path = "$INDEX_DIR" }

[dependencies.compat]
cfg = "1.0.0"

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

echo "OK"
