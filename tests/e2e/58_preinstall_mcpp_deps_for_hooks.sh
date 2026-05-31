#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Packages installed from an mcpp index may need their mcpp.deps available
# before the package install hook runs. Library deps belong in mcpp.deps; only
# hook-time tools should be declared as xpm deps.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/c"

cat > "$INDEX_DIR/pkgs/c/compat.proto.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "compat",
    name = "compat.proto",
    description = "Protocol helper dependency",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/proto-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/proto.c" },
        targets = { ["proto"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

cat > "$INDEX_DIR/pkgs/c/compat.appdep.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "compat",
    name = "compat.appdep",
    description = "Package whose install hook needs mcpp deps",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/appdep-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/appdep.c" },
        targets = { ["appdep"] = { kind = "lib" } },
        deps = {
            ["compat.proto"] = "1.0.0",
        },
    },
}
EOF

mkdir -p "$TMP/fake-bin"
FAKE_REGISTRY="$TMP/fake-registry"
FAKE_DIRECT_LOG="$TMP/fake-xlings-direct.log"
mkdir -p "$FAKE_REGISTRY/data"
USER_MCPP="${HOME}/.mcpp"
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
    exit 0
fi

if [[ "${1:-}" == "interface" && "${2:-}" == "install_packages" ]]; then
    echo "interface install should not be used for project path indices" >&2
    exit 31
fi

if [[ "${1:-}" == "install" ]]; then
    printf '%s\n' "$*" >> "${FAKE_XLINGS_DIRECT_LOG:?}"
    if [[ ! -d "${XLINGS_PROJECT_DIR:?}/.xlings/data/compat/pkgs" \
       && ! -d "${XLINGS_PROJECT_DIR:?}/data/compat/pkgs" ]]; then
        echo "missing project local path index link" >&2
        find "${XLINGS_PROJECT_DIR:?}" -maxdepth 4 -type d -print >&2 2>/dev/null || true
        exit 23
    fi

    case " $* " in
        *" compat:compat.proto@1.0.0 "*)
            install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.proto/1.0.0"
            mkdir -p "$install_root/src"
            cat > "$install_root/src/proto.c" <<'SRC'
int proto_value(void) {
    return 42;
}
SRC
            exit 0
            ;;
        *" compat:compat.appdep@1.0.0 "*)
            proto_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.proto/1.0.0"
            if [[ ! -d "$proto_root" ]]; then
                echo "mcpp.deps were not installed before appdep install hook" >&2
                exit 24
            fi
            install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.appdep/1.0.0"
            mkdir -p "$install_root/src"
            cat > "$install_root/src/appdep.c" <<'SRC'
extern int proto_value(void);
int app_value(void) {
    return proto_value();
}
SRC
            exit 0
            ;;
    esac
fi

exit 0
EOF
chmod +x "$TMP/fake-bin/xlings"

mkdir -p "$TMP/project/app/src"
cd "$TMP/project/app"

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

cat > src/app.cpp <<'EOF'
extern "C" int app_value(void);
int app_smoke(void) {
    return app_value();
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
compat = { path = "$INDEX_DIR" }

[dependencies.compat]
appdep = "1.0.0"

[targets.app]
kind = "lib"
EOF

if ! FAKE_XLINGS_DIRECT_LOG="$FAKE_DIRECT_LOG" "$MCPP" build > build.log 2>&1; then
    cat build.log
    exit 1
fi

if ! grep -Fq 'install compat:compat.proto@1.0.0 -y' "$FAKE_DIRECT_LOG"; then
    echo "FAIL: compat.proto was not installed from mcpp.deps"
    cat "$FAKE_DIRECT_LOG" 2>/dev/null || true
    exit 1
fi

if ! awk '
    /install compat:compat.proto@1\.0\.0 -y/ { proto = NR }
    /install compat:compat.appdep@1\.0\.0 -y/ { appdep = NR }
    END { exit !(proto > 0 && appdep > 0 && proto < appdep) }
' "$FAKE_DIRECT_LOG"; then
    echo "FAIL: mcpp.deps should be installed before dependent package hook"
    cat "$FAKE_DIRECT_LOG" 2>/dev/null || true
    exit 1
fi

echo "OK"
