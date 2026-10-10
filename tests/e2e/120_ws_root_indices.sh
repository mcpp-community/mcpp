#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# #224: root-anchored inheritance.
#   1. A workspace ROOT declares `[indices] x = { path = "..." }` (a
#      relative path) ONCE. A member with NO [indices] section of its own
#      inherits it and must resolve the relative path against the
#      WORKSPACE ROOT, not `<root>/<member>/...` — otherwise every member
#      would need its own `../`-prefixed re-declaration.
#   2. `[workspace.dependencies] ylib = { path = "..." }` (also relative to
#      the workspace root) is usable by a member via `ylib.workspace = true`
#      — previously `merge_workspace_deps` only propagated `version`, so a
#      path-form workspace dependency was silently dropped and failed to
#      resolve.
#   3. A member that declares its own `[indices]` with a relative path wrote
#      the path in ITS directory. The plan's root is the workspace root, so
#      the member's path is anchored at the member (2026.9.29.1 resolved it
#      against the workspace root and found no index). The two members name
#      one tree, so they are one configuration.
#   4. COMPAT(workspace-position): the root's own `[indices]` reaches the
#      members by POSITION, with a warning naming `[workspace.indices]`, until
#      mcpp 1.0.0 (SPEC-004 §9.10). Kept in this spelling as that coverage;
#      tests/e2e/893 covers `[workspace.indices]`.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws"
cd "$TMP/ws"

# ── A project-local index, declared relative to the WORKSPACE ROOT ──────
mkdir -p local-index/pkgs/x
cat > local-index/pkgs/x/x.widget2.lua <<'EOF'
package = {
    spec = "1",
    namespace = "x",
    name = "x.widget2",
    description = "Namespaced package reachable only via the workspace-root-anchored index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/widget2-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/widget2.cppm" },
        targets = { ["widget2"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

# ── A path-form workspace dependency, also relative to the WORKSPACE ROOT ──
mkdir -p ylib/src
cat > ylib/mcpp.toml <<'EOF'
[package]
name = "ylib"
version = "0.1.0"

[targets.ylib]
kind = "lib"
EOF
cat > ylib/src/ylib.cppm <<'EOF'
export module ylib;

export int ylib_value() {
    return 7;
}
EOF

# ── Workspace root: virtual, one member, no re-declaration required ─────
cat > mcpp.toml <<EOF
[workspace]
members = ["member-a", "member-b"]

[indices]
x = { path = "local-index" }

[workspace.dependencies]
ylib = { path = "ylib" }
EOF

# A workspace's project-local payloads and its lock live at its root: the
# workspace is the unit of build (workspace design 2026-09-29 §15).
mkdir -p member-a/src \
         .mcpp/.xlings/data/xpkgs/x.widget2/1.0.0/src
cat > .mcpp/.xlings/data/xpkgs/x.widget2/1.0.0/src/widget2.cppm <<'EOF'
export module widget2;

export int widget2_value() {
    return 35;
}
EOF

# Deliberately NO [indices] section here — must inherit from the root and
# resolve "local-index" against the WORKSPACE ROOT, not member-a/local-index.
cat > member-a/mcpp.toml <<'EOF'
[package]
name = "member-a"
version = "0.1.0"

[dependencies.x]
widget2 = "1.0.0"

[dependencies]
ylib = { workspace = true }

[targets.member-a]
kind = "bin"
main = "src/main.cpp"
EOF

cat > member-a/src/main.cpp <<'EOF'
import widget2;
import ylib;

int main() {
    return (widget2_value() + ylib_value() == 42) ? 0 : 1;
}
EOF

mkdir -p member-b/src
cat > member-b/mcpp.toml <<'EOF'
[indices]
x = { path = "../local-index" }

[package]
name = "member-b"
version = "0.1.0"

[dependencies.x]
widget2 = "1.0.0"
EOF
cat > member-b/src/main.cpp <<'EOF'
import widget2;

int main() {
    return widget2_value() == 35 ? 0 : 1;
}
EOF

"$MCPP" build -p member-a > build.log 2>&1 || {
    cat build.log
    echo "FAIL: member did not resolve root-anchored [indices]/[workspace.dependencies] path"
    exit 1
}

grep -q '\[indices\] on the workspace root reaches every member by its position' build.log || {
    cat build.log
    echo "FAIL: the position reading of [indices] says nothing (COMPAT(workspace-position))"
    exit 1
}
grep -q '\[workspace.indices\]' build.log || {
    cat build.log
    echo "FAIL: the warning does not name the canonical spelling"
    exit 1
}

"$MCPP" run -p member-a > run.log 2>&1 || {
    cat run.log
    echo "FAIL: run failed"
    exit 1
}

"$MCPP" build -p member-b > build-b.log 2>&1 || {
    cat build-b.log
    echo "FAIL: a member's own relative [indices].path is not anchored at the member"
    exit 1
}
"$MCPP" run -p member-b > run-b.log 2>&1 || {
    cat run-b.log
    echo "FAIL: member-b's program failed"
    exit 1
}
"$MCPP" build --workspace > build-ws.log 2>&1 || {
    cat build-ws.log
    echo "FAIL: --workspace"
    exit 1
}
n=$(find target -name build.ninja | wc -l)
[[ "$n" == 1 ]] || {
    find target -name build.ninja
    echo "FAIL: members naming one index tree are $n configurations, expected 1"
    exit 1
}

grep -q '\[package\."x.widget2"\]' mcpp.lock || {
    cat mcpp.lock 2>/dev/null || true
    echo "FAIL: expected x.widget2 lock entry"
    exit 1
}

echo "OK"
