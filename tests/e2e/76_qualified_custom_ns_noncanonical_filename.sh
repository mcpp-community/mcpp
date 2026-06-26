#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# Regression for the identity-first resolution gap
# (.agents/docs/2026-06-26-identity-first-resolution-no-filename.md).
#
# Production trigger: package `aimol.tensorvia-cpu` declares
#   namespace = "aimol", name = "tensorvia-cpu"
# and is hosted in the BUILTIN mcpplibs index (NOT a [indices] entry), filed under
# the NON-canonical bare filename `pkgs/t/tensorvia-cpu.lua` (canonical would be
# `pkgs/a/aimol.tensorvia-cpu.lua`). A qualified request `aimol.tensorvia-cpu`
# must resolve by the descriptor's DECLARED (ns, name) — the filename is not a key.
#
# This is the exact intersection the prior suite never crossed at once:
#   builtin index  ×  non-canonical filename  ×  qualified multi-candidate request.
# Fixed by making selectDependencyCandidate identity-first (it now locates each
# candidate by the descriptor's declared (ns, name) via read_xpkg_lua* instead of
# probing the canonical filename `<ns>.<short>.lua`).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$MCPP_HOME/registry/data/mcpplibs"
# Observed real install layout is <ns>-x-<short>: aimol-x-tensorvia-cpu.
PKG_ROOT="$MCPP_HOME/registry/data/xpkgs/aimol-x-tensorvia-cpu/0.1.1"
mkdir -p "$INDEX_DIR/pkgs/t" "$PKG_ROOT/src"
printf 'ok\n' > "$INDEX_DIR/.mcpp-index-updated"

# Custom namespace "aimol", bare name "tensorvia-cpu", filed under the BARE
# filename in the mcpplibs index — filename does not encode the namespace.
cat > "$INDEX_DIR/pkgs/t/tensorvia-cpu.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "aimol",
    name = "tensorvia-cpu",
    description = "Custom-namespace package filed under a non-canonical filename",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["0.1.1"] = {
                url = "https://example.invalid/tensorvia-cpu-0.1.1.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/tensorvia.cppm" },
        targets = { ["tensorvia-cpu"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

cat > "$PKG_ROOT/src/tensorvia.cppm" <<'EOF'
export module tensorvia.cpu;

export int tensorvia_value() {
    return 42;
}
EOF
printf 'ok\n' > "$PKG_ROOT/.mcpp_ok"

mkdir -p "$TMP/project/app/src"
cd "$TMP/project/app"

cat > src/main.cpp <<'EOF'
import tensorvia.cpu;

int main() {
    return tensorvia_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
aimol.tensorvia-cpu = "0.1.1"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

# (1) Qualified custom-ns request must resolve despite the non-canonical filename.
"$MCPP" build > build.log 2>&1 || {
    echo "FAIL: aimol.tensorvia-cpu did not resolve (identity-first regression)"
    cat build.log
    exit 1
}
"$MCPP" run > run.log 2>&1 || { cat run.log; exit 1; }

# (2) The resolved identity must record the declared namespace, not mcpplibs.aimol.
grep -q 'namespace = "aimol"' mcpp.lock || {
    cat mcpp.lock
    echo "FAIL: lock must record resolved namespace aimol"
    exit 1
}
if grep -q 'mcpplibs.aimol' mcpp.lock; then
    cat mcpp.lock
    echo "FAIL: front candidate mcpplibs.aimol leaked into the lock"
    exit 1
fi

# (3) A genuinely wrong namespace must be a clean not-found, not a silent match.
cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
mcpplibs.tensorvia-cpu = "0.1.1"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
rm -f mcpp.lock
if "$MCPP" build > wrong.log 2>&1; then
    echo "FAIL: mcpplibs.tensorvia-cpu must NOT resolve (package is aimol-namespaced)"
    cat wrong.log
    exit 1
fi

echo "OK"
