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

# Also verify the install target used for a clean local path index. The
# package name in the index is compat.cfg, so xlings must receive the fully
# qualified package name after the index selector: compat:compat.cfg@1.0.0.
mkdir -p "$TMP/fake-bin"
FAKE_REGISTRY="$TMP/fake-registry"
FAKE_LOG="$TMP/fake-xlings.log"
FAKE_DIRECT_LOG="$TMP/fake-xlings-direct.log"
mkdir -p "$FAKE_REGISTRY/data"
mkdir -p "$FAKE_REGISTRY/data/xim-pkgindex/pkgs/p"
cat > "$FAKE_REGISTRY/data/xim-pkgindex/pkgs/p/python.lua" <<'EOF'
package = {
    spec = "1",
    name = "python",
    xpm = {
        linux = {
            ["3"] = {
                url = "https://example.invalid/python.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
EOF
if [[ -d "$USER_MCPP/registry/data/xpkgs" ]]; then
    ln -s "$USER_MCPP/registry/data/xpkgs" "$FAKE_REGISTRY/data/xpkgs"
fi
cat > "$TMP/fake-bin/xlings" <<'EOF'
#!/usr/bin/env bash
set -e

if [[ "${1:-}" == "self" && "${2:-}" == "init" ]]; then
    mkdir -p "${XLINGS_HOME:?}/subos/default"
    printf '{}\n' > "$XLINGS_HOME/subos/default/.xlings.json"
    exit 0
fi

if [[ "${1:-}" == "update" ]]; then
    printf 'update\n' > "${FAKE_XLINGS_UPDATE_LOG:?}"
    exit 0
fi

# Project/custom-index deps install through the NDJSON interface (so the live
# download-progress UI renders). The capability honors XLINGS_PROJECT_DIR and
# installs into the project-local data root — assert that here and emit a
# minimal NDJSON stream (a download_progress event + result).
if [[ "${1:-}" == "interface" && "${2:-}" == "install_packages" ]]; then
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "--args" ]]; then
            printf '%s\n' "$2" > "${FAKE_XLINGS_LOG:?}"
            break
        fi
        shift
    done
    # The official global `xim` index must NOT be injected into the project's
    # .xlings.json — it's a global-default index (global IS the default scope);
    # injecting it project-scoped mis-scopes every xim:* tool into the project
    # store. Only user-declared local custom indices belong here.
    if grep -q '"name": "xim"' "${XLINGS_PROJECT_DIR:?}/.xlings.json"; then
        echo "official xim index must NOT be injected into project .xlings.json" >&2
        cat "${XLINGS_PROJECT_DIR:?}/.xlings.json" >&2 2>/dev/null || true
        exit 24
    fi
    if [[ ! -d "${XLINGS_PROJECT_DIR:?}/.xlings/data/compat/pkgs" \
       && ! -d "${XLINGS_PROJECT_DIR:?}/data/compat/pkgs" ]]; then
        echo "missing project local path index link" >&2
        find "${XLINGS_PROJECT_DIR:?}" -maxdepth 4 -type d -print >&2 2>/dev/null || true
        exit 23
    fi
    install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.cfg/1.0.0"
    mkdir -p "$install_root/src"
    cat > "$install_root/src/cfg.c" <<'SRC'
int cfg_value(void) {
    return 42;
}
SRC
    printf '{"kind":"data","dataKind":"download_progress","payload":{"elapsedSec":0.1,"files":[{"name":"compat:compat.cfg@1.0.0","downloadedBytes":0,"totalBytes":0,"started":true,"finished":false}]}}\n'
    printf '{"kind":"data","dataKind":"download_progress","payload":{"elapsedSec":0.3,"files":[{"name":"compat:compat.cfg@1.0.0","downloadedBytes":1024,"totalBytes":1024,"started":true,"finished":true}]}}\n'
    printf '{"kind":"result","exitCode":0}\n'
    exit 0
fi

if [[ "${1:-}" == "install" ]]; then
    printf '%s\n' "$*" > "${FAKE_XLINGS_DIRECT_LOG:?}"
    exit 0
fi

exit 0
EOF
chmod +x "$TMP/fake-bin/xlings"

cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
binary = "$TMP/fake-bin/xlings"
home = "$FAKE_REGISTRY"

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

mkdir -p "$TMP/project/clean/src"
cd "$TMP/project/clean"

cat > src/clean.cpp <<'EOF'
extern "C" int cfg_value(void);
int clean_value(void) {
    return cfg_value();
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "clean"
version = "0.1.0"

[indices]
compat = { path = "$INDEX_DIR" }

[dependencies.compat]
cfg = "1.0.0"

[targets.clean]
kind = "lib"
EOF

UPDATE_LOG="$TMP/fake-xlings-update.log"
if ! FAKE_XLINGS_LOG="$FAKE_LOG" \
     FAKE_XLINGS_DIRECT_LOG="$FAKE_DIRECT_LOG" \
     FAKE_XLINGS_UPDATE_LOG="$UPDATE_LOG" \
     "$MCPP" build > fetch.log 2>&1; then
    echo "FAIL: clean local path dependency install failed"
    cat fetch.log
    exit 1
fi

if [[ -f "$UPDATE_LOG" ]]; then
    echo "FAIL: local path index dependency should not refresh the builtin package index"
    cat fetch.log
    exit 1
fi

# Project/custom-index deps install through the NDJSON interface so the live
# download-progress UI renders (the capability honors XLINGS_PROJECT_DIR and
# still lands the package in the project-local data root).
if [[ -f "$FAKE_DIRECT_LOG" ]]; then
    echo "FAIL: project local path install should use the NDJSON interface, not direct xlings install"
    cat "$FAKE_DIRECT_LOG"
    cat fetch.log
    exit 1
fi

grep -Fq 'compat:compat.cfg@1.0.0' "$FAKE_LOG" || {
    echo "FAIL: clean local path install target should use the interface with the full package name"
    echo "recorded:"
    cat "$FAKE_LOG" 2>/dev/null || true
    echo "build log:"
    cat fetch.log
    exit 1
}

echo "OK"
