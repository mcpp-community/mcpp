#!/usr/bin/env bash
# requires:
# 880 -- mcpp#760: a member with shared and bin targets builds an independent
# executable from its own implementation, not from its sibling shared library.
#
# A module interface, a separate implementation unit and a static dependency
# must all reach the executable. Another member still consumes the shared
# target. Workspace selection, shipped artifacts and host tools keep this rule.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

# Share the installed tool payloads, but keep the build and package caches
# private. The fixture has no registry package dependencies; the default host
# toolchain is resolved through the existing registry, as in the E2E runner.
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/mcpp-home"
mkdir -p "$MCPP_HOME" "$TMP/ws"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac
bin_of() { find target -path '*/bin/*' -name "$1$EXE" -type f | head -1; }

cd "$TMP/ws"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["dual", "client", "support"]
EOF
mkdir -p common/src dual/src client/src support/src
cat > support/mcpp.toml <<'EOF'
[package]
name = "support"
version = "0.1.0"

[targets.support]
kind = "shared"
EOF
printf 'export module t880_support;\nexport int delta() { return 1; }\n' > support/src/support.cppm
cat > common/mcpp.toml <<'EOF'
[package]
name = "common"
version = "0.1.0"

[targets.common]
kind = "lib"
EOF
printf 'export module t880_base;\nexport int base_value() { return 41; }\n' > common/src/base.cppm
cat > dual/mcpp.toml <<'EOF'
[package]
name = "dual"
version = "0.1.0"

[dependencies]
common = { path = "../common" }
support = { path = "../support" }

[targets.dual_dll]
kind = "shared"

[targets.dual]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'export module t880_dual;\nexport int interface_value() { return 1; }\nexport int answer();\n' > dual/src/dual.cppm
printf 'module t880_dual;\nimport t880_base;\nimport t880_support;\nint answer() { return base_value() + delta(); }\n' > dual/src/impl.cpp
printf 'import t880_dual;\nint main() { return answer() == 42 && interface_value() == 1 ? 0 : 1; }\n' > dual/src/main.cpp
cat > client/mcpp.toml <<'EOF'
[package]
name = "client"
version = "0.1.0"

[dependencies]
dual = { path = "../dual" }

[targets.client]
kind = "bin"
main = "src/main.cpp"
EOF
cp dual/src/main.cpp client/src/main.cpp

"$MCPP" build --workspace > workspace.log 2>&1 || fail "the workspace did not build" workspace.log
dual=$(bin_of dual)
client=$(bin_of client)
[ -n "$dual" ] && [ -n "$client" ] || fail "a member's executable is missing" workspace.log
"$dual" || fail "the owner's executable did not run"
"$client" || fail "the shared-library consumer did not run"

# Remove every placement of the sibling library. A client that imports it must
# fail, while the owner's executable must still run: mere success with both
# products beside it would also accept the import-library workaround for #760.
mkdir hidden
n=0
while IFS= read -r library; do
    n=$((n + 1))
    mv "$library" "hidden/$n"
done < <(find target -path '*/bin/*' \( -name '*dual_dll.dll' -o -name '*dual_dll.so*' -o -name '*dual_dll.dylib' \))
[ "$n" -gt 0 ] || fail "the sibling shared library was not built" workspace.log
"$dual" || fail "the owner's executable depends on its sibling shared library"
if "$client" > client.log 2>&1; then
    fail "the external consumer did not use the shared library" client.log
fi

"$MCPP" build -p dual > selected.log 2>&1 || fail "the selected member did not build" selected.log
"$(bin_of dual)" || fail "the selected member's executable did not run"

# Request the same executable as an artifact, rather than selecting its owner.
# The feature gate must be activated by the dependency that requests it.
cat >> dual/mcpp.toml <<'EOF'
required_features = ["exe"]

[features]
exe = []
EOF
mkdir -p shipped/src
cat > shipped/mcpp.toml <<'EOF'
[package]
name = "shipped"
version = "0.1.0"

[dependencies]
dual = { path = "../dual", artifacts = ["dual"], features = ["exe"] }
support = { path = "../support" }

[targets.shipped]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'import t880_support;\nint main() { return delta() == 1 ? 0 : 1; }\n' > shipped/src/main.cpp
cat > mcpp.toml <<'EOF'
[workspace]
members = ["dual", "client", "support", "shipped"]
EOF
"$MCPP" build -p shipped > artifact.log 2>&1 || fail "the requested artifact did not build" artifact.log
artifact=$(find target -path "*/bin/shipped/dual$EXE" -type f | head -1)
[ -n "$artifact" ] || fail "the requested artifact was not placed beside its consumer" artifact.log
"$artifact" || fail "the requested artifact did not run"
"$(bin_of shipped)" || fail "the artifact's consumer did not run"
"$MCPP" build --workspace > artifact-workspace.log 2>&1 || fail "the workspace with an artifact request did not build" artifact-workspace.log
"$artifact" || fail "the workspace's requested artifact did not run"
"$dual" || fail "the workspace's owner executable did not run"
n=0
while IFS= read -r library; do
    n=$((n + 1))
    mv "$library" "hidden/artifact-$n"
done < <(find target -path '*/bin/*' \( -name '*dual_dll.dll' -o -name '*dual_dll.so*' -o -name '*dual_dll.dylib' \))
"$artifact" || fail "the requested artifact depends on its sibling shared library"

# A host tool is rooted at its own package and published separately. Use a
# static dependency here: external shared-tool runtime deployment is a separate
# contract, while this test checks independence from the tool's own sibling.
mkdir -p "$TMP/tooldual/src" "$TMP/toolapp/src"
cp dual/src/dual.cppm "$TMP/tooldual/src/dual.cppm"
cp dual/src/main.cpp "$TMP/tooldual/src/main.cpp"
printf 'module t880_dual;\nimport t880_base;\nint answer() { return base_value() + 1; }\n' > "$TMP/tooldual/src/impl.cpp"
cat > "$TMP/tooldual/mcpp.toml" <<'EOF'
[package]
name = "tooldual"
version = "0.1.0"

[dependencies]
common = { path = "../ws/common" }

[features]
exe = []

[targets.dual_dll]
kind = "shared"

[targets.dual]
kind = "bin"
main = "src/main.cpp"
required_features = ["exe"]
EOF
cat > "$TMP/toolapp/mcpp.toml" <<'EOF'
[package]
name = "toolapp"
version = "0.1.0"

[build-dependencies]
tooldual = { path = "../tooldual", tools = ["dual"], features = ["exe"] }
EOF
printf 'int main() { return 0; }\n' > "$TMP/toolapp/src/main.cpp"
cat > "$TMP/toolapp/build.mcpp" <<'EOF'
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("tooldual", "dual");
    if (!tool || !*tool) return 1;
    return std::system((std::string("\"") + tool + "\"").c_str()) == 0 ? 0 : 1;
}
EOF
cd "$TMP/toolapp"
"$MCPP" build > tool.log 2>&1 || fail "the host tool did not build and run" tool.log
"$(bin_of toolapp)" || fail "the host tool's consumer did not run"
# A target-side edge must retain the same provider's shared product even when
# it is also requested as a build-time tool.
cat >> mcpp.toml <<'EOF'

[dependencies]
tooldual = { path = "../tooldual" }
EOF
cp "$TMP/tooldual/src/main.cpp" src/main.cpp
"$MCPP" build > dual-role.log 2>&1 || fail "the dual-role provider did not build" dual-role.log
"$(bin_of toolapp)" || fail "the dual-role provider's consumer did not run"
[ -n "$(find target -path '*/bin/*' \( -name '*dual_dll.dll' -o -name '*dual_dll.so*' -o -name '*dual_dll.dylib' \) -type f | head -1)" ] || fail "the dual-role provider's shared library is missing" dual-role.log


# A static placed in the owner's own image (#766). `common` is reached only
# through `dual`, which produces a shared image, so `common` is placed in that
# image. `dual`'s executable links `common`'s objects itself and must then also
# link `common`'s own shared dependency. `client` links `dual`'s image, which
# holds `common`, and not `common`'s objects a second time.
mkdir -p "$TMP/ws2"
cd "$TMP/ws2"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["dual", "client", "support"]
EOF
mkdir -p common/src dual/src client/src support/src
cp "$TMP/ws/support/mcpp.toml" support/mcpp.toml
cp "$TMP/ws/support/src/support.cppm" support/src/support.cppm
cat > common/mcpp.toml <<'EOF'
[package]
name = "common"
version = "0.1.0"

[dependencies]
support = { path = "../support" }

[targets.common]
kind = "lib"
EOF
printf 'export module t880_base;\nimport t880_support;\nexport int base_value() { return 40 + delta(); }\n' > common/src/base.cppm
cat > dual/mcpp.toml <<'EOF'
[package]
name = "dual"
version = "0.1.0"

[dependencies]
common = { path = "../common" }

[targets.dual_dll]
kind = "shared"

[targets.dual]
kind = "bin"
main = "src/main.cpp"
EOF
cp "$TMP/ws/dual/src/dual.cppm" dual/src/dual.cppm
printf 'module t880_dual;\nimport t880_base;\nint answer() { return base_value() + 1; }\n' > dual/src/impl.cpp
cp "$TMP/ws/dual/src/main.cpp" dual/src/main.cpp
cp "$TMP/ws/client/mcpp.toml" client/mcpp.toml
cp "$TMP/ws/client/src/main.cpp" client/src/main.cpp
"$MCPP" build --workspace > placed.log 2>&1 || fail "a static placed in the owner's image lost its shared dependency" placed.log
"$(bin_of dual)" || fail "the owner's executable with a placed static did not run"
"$(bin_of client)" || fail "the consumer of an image holding a placed static did not run"

echo "PASS: 880_a_members_executable_keeps_its_own_implementation"
