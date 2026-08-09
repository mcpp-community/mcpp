#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# A compact dotted selector used to try mcpplibs.<ns> first. During the one
# migration release, an existing lock keeps that already-selected identity;
# without the lock, the selector is exact. Both identities exist here so index
# order cannot accidentally make the assertion pass.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
APP="$TMP/app"
mkdir -p "$INDEX_DIR/pkgs/c" "$INDEX_DIR/pkgs/m" "$APP/src"

descriptor() {
    local ns=$1
    local desc=$2
    cat <<EOF
package = {
    spec = "1",
    namespace = "$ns",
    name = "lua",
    description = "$desc",
    licenses = {"MIT"},
    type = "package",
    xpm = { linux = { ["1.0.0"] = {
        url = "https://example.invalid/$ns-lua.tar.gz",
        sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    } } },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/lua.cppm" },
        targets = { ["lua"] = { kind = "lib" } },
        deps = {},
    },
}
EOF
}

descriptor capi "new exact identity" > "$INDEX_DIR/pkgs/c/capi.lua.lua"
descriptor mcpplibs.capi "old prefixed identity" \
    > "$INDEX_DIR/pkgs/m/mcpplibs.capi.lua.lua"

OLD_ROOT="$APP/.mcpp/.xlings/data/xpkgs/mcpplibs.capi-x-lua/1.0.0"
NEW_ROOT="$APP/.mcpp/.xlings/data/xpkgs/capi-x-lua/1.0.0"
mkdir -p "$OLD_ROOT/src" "$NEW_ROOT/src"
cat > "$OLD_ROOT/src/lua.cppm" <<'EOF'
export module selected.lua;
export int selected_value() { return 41; }
EOF
cat > "$NEW_ROOT/src/lua.cppm" <<'EOF'
export module selected.lua;
export int selected_value() { return 42; }
EOF
printf 'ok\n' > "$OLD_ROOT/.mcpp_ok"
printf 'ok\n' > "$NEW_ROOT/.mcpp_ok"

cat > "$APP/src/main.cpp" <<'EOF'
import selected.lua;
int main() { return selected_value() == 41 ? 0 : 1; }
EOF
cat > "$APP/mcpp.toml" <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
default = { path = "$INDEX_DIR_HOST" }
capi = { path = "$INDEX_DIR_HOST" }

[dependencies]
capi.lua = "1.0.0"
EOF
cat > "$APP/mcpp.lock" <<'EOF'
# Existing pre-exact-selector resolution anchor.
version = 2

[package."capi.lua"]
namespace = "mcpplibs.capi"
version = "1.0.0"
source = "index+mcpplibs.capi@1.0.0"
hash = "fnv1a:migration-fixture"
EOF

cd "$APP"
"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }
grep -q "keeping the locked identity" build.log || {
    cat build.log
    echo "expected selector migration warning"
    exit 1
}
grep -q "mcpplibs.capi.lua" build.log || {
    cat build.log
    echo "warning must include the locked selector"
    exit 1
}
"$MCPP" run > run.log 2>&1 || { cat run.log; exit 1; }
grep -q 'namespace = "mcpplibs.capi"' mcpp.lock || {
    cat mcpp.lock
    echo "lock identity changed during migration"
    exit 1
}

echo "OK"
