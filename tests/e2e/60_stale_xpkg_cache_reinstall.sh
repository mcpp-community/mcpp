#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Version dependencies must not reuse stale xpkg directories that predate
# mcpp's .mcpp_ok install marker. Such directories can have an old extracted
# layout that no longer matches the package index's mcpp metadata.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/c"
cat > "$INDEX_DIR/pkgs/c/compat.stale.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "compat",
    name = "compat.stale",
    description = "stale cache reinstall test package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/stale-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/stale.c" },
        targets = { ["stale"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

mkdir -p "$TMP/fake-bin"
FAKE_INSTALL_LOG="$TMP/fake-install.log"
cat > "$TMP/fake-bin/xlings" <<'EOF'
#!/usr/bin/env bash
set -e

if [[ "${1:-}" == "self" && "${2:-}" == "init" ]]; then
    mkdir -p "${XLINGS_HOME:?}/subos/default"
    printf '{}\n' > "$XLINGS_HOME/subos/default/.xlings.json"
    exit 0
fi

if [[ "${1:-}" == "update" ]]; then
    exit 0
fi

# Project/custom-index deps install through the NDJSON interface (so the live
# download-progress UI renders); it installs into the project-local data root.
if [[ "${1:-}" == "interface" && "${2:-}" == "install_packages" ]]; then
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "--args" ]]; then printf '%s\n' "$2" > "${FAKE_INSTALL_LOG:?}"; break; fi
        shift
    done
    install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.stale/1.0.0"
    mkdir -p "$install_root/src"
    cat > "$install_root/src/stale.c" <<'SRC'
int stale_value(void) {
    return 42;
}
SRC
    printf '{"kind":"data","dataKind":"download_progress","payload":{"elapsedSec":0.2,"files":[{"name":"compat:compat.stale@1.0.0","downloadedBytes":2048,"totalBytes":2048,"started":true,"finished":true}]}}\n'
    printf '{"kind":"result","exitCode":0}\n'
    exit 0
fi

if [[ "${1:-}" == "install" ]]; then
    echo "direct install should not be used for project path indices" >&2
    exit 31
fi

exit 0
EOF
chmod +x "$TMP/fake-bin/xlings"

cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
binary = "$TMP/fake-bin/xlings"
home = "$MCPP_HOME/registry"

[index]
default = "mcpplibs"

[index.repos."mcpplibs"]
url = "https://github.com/mcpp-community/mcpp-index.git"

[cache]
search_ttl_seconds = 3600

[build]
default_jobs = 0
default_backend = "ninja"

[toolchain]
default = "gcc@16.1.0"
EOF

mkdir -p "$TMP/project/app/src" \
         "$TMP/project/app/.mcpp/.xlings/data/xpkgs/compat-x-compat.stale/1.0.0/stale-1.0.0/src"

cd "$TMP/project/app"

# Old cache layout: content sits below a nested extracted directory and has no
# .mcpp_ok marker. The current index metadata expects src/stale.c at version
# root, so reusing this directory would omit the dependency object.
cat > .mcpp/.xlings/data/xpkgs/compat-x-compat.stale/1.0.0/stale-1.0.0/src/stale.c <<'EOF'
int stale_value(void) {
    return 13;
}
EOF

cat > src/main.cpp <<'EOF'
extern "C" int stale_value(void);
int main() {
    return stale_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
compat = { path = "$INDEX_DIR" }

[dependencies.compat]
stale = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

if ! FAKE_INSTALL_LOG="$FAKE_INSTALL_LOG" "$MCPP" build > build.log 2>&1; then
    cat build.log
    exit 1
fi

grep -Fq 'compat:compat.stale@1.0.0' "$FAKE_INSTALL_LOG" || {
    echo "FAIL: stale unmarked xpkg directory should trigger reinstall"
    cat build.log
    exit 1
}

test -f .mcpp/.xlings/data/xpkgs/compat-x-compat.stale/1.0.0/.mcpp_ok || {
    echo "FAIL: successful reinstall should be marked complete"
    find .mcpp/.xlings/data/xpkgs/compat-x-compat.stale/1.0.0 -maxdepth 2 -print
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    exit 1
}

echo "OK"
