#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# R6: `[indices] default = { path = ... }` (also spellable `"" = {...}`)
# redirects the DEFAULT namespace — bare `gizmo = "1.0.0"` deps with no
# namespace prefix — to a local checkout, instead of the two hardcoded
# short-circuits (prepare.cppm usesBuiltinIndex / findIndexForNs) that used
# to route the default namespace straight to the builtin index regardless
# of [indices]. The package is declared ONLY in a project-local index dir
# that lives outside $MCPP_HOME (never seeded into the global/builtin
# registry location), so a build that still fell through to the builtin
# short-circuit would fail to resolve it.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# A local index directory, deliberately NOT under $MCPP_HOME — it must be
# reached only via the [indices] default = {...} redirect.
INDEX_DIR="$TMP/local-index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/g"
cat > "$INDEX_DIR/pkgs/g/gizmo.lua" <<'EOF'
package = {
    spec = "1",
    name = "gizmo",
    description = "Default-namespace package served only via a redirected index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/gizmo-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/gizmo.cppm" },
        targets = { ["gizmo"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

mkdir -p "$TMP/project/app/src" \
         "$TMP/project/app/.mcpp/.xlings/data/xpkgs/gizmo/1.0.0/src"
cd "$TMP/project/app"

cat > .mcpp/.xlings/data/xpkgs/gizmo/1.0.0/src/gizmo.cppm <<'EOF'
export module gizmo;

export int gizmo_value() {
    return 42;
}
EOF

cat > src/main.cpp <<'EOF'
import gizmo;

int main() {
    return gizmo_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
default = { path = "$INDEX_DIR_HOST" }

[dependencies]
gizmo = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: build did not resolve default-namespace dep from the redirected local index"
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    echo "FAIL: run failed"
    exit 1
}

grep -q '\[package\."gizmo"\]' mcpp.lock || {
    cat mcpp.lock
    echo "FAIL: expected gizmo package lock entry"
    exit 1
}

# ── "" as an alternate spelling of the redirect key ─────────────────────
mkdir -p "$TMP/project/app2/src"
cd "$TMP/project/app2"

cat > src/main.cpp <<'EOF'
import gizmo;

int main() {
    return gizmo_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app2"
version = "0.1.0"

[indices]
"" = { path = "$INDEX_DIR_HOST" }

[dependencies]
gizmo = "1.0.0"

[targets.app2]
kind = "bin"
main = "src/main.cpp"
EOF

mkdir -p .mcpp/.xlings/data/xpkgs/gizmo/1.0.0/src
cat > .mcpp/.xlings/data/xpkgs/gizmo/1.0.0/src/gizmo.cppm <<'EOF'
export module gizmo;

export int gizmo_value() {
    return 42;
}
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo 'FAIL: [indices] "" = {...} did not redirect the default namespace'
    exit 1
}

echo "OK"
