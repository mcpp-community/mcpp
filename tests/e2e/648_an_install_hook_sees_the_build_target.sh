#!/usr/bin/env bash
# requires: elf
# 648_an_install_hook_sees_the_build_target.sh -- #613: a dependency's install
# hook receives the build's target under the names a build program uses, and the
# two toolchain names present and empty.
#
# The toolchain is resolved after the dependency graph, so no compiler or
# standard library exists when a dependency installs, and the values are emitted
# empty rather than guessed. The run exports both toolchain names first: a hook
# that reads a non-empty value read one inherited from its parent, which the
# always-emitted rule exists to prevent.
#
# The package below has no download. Its `install()` writes the module mcpp
# compiles next, with the values it read compiled in, and the consumer prints
# them. What is compared is therefore what the hook saw, carried in the artefact.
# The composition itself is unit-tested on every platform (test_install_hook_env).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/local-index/pkgs/h" "$TMP/proj/src"
cd "$TMP/proj"

cat > local-index/pkgs/h/acme.hookprobe.lua <<'LUA'
package = {
    spec = "1",
    namespace = "acme",
    name = "hookprobe",
    description = "Compiles in what its install hook saw",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = { } },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = { } },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = { } },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/hookprobe.cppm" },
        targets = { ["hookprobe"] = { kind = "lib" } },
        deps = {},
    },
}

import("xim.libxpkg.pkginfo")

function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, "src"))
    local function value(name) return os.getenv(name) or "" end
    io.writefile(path.join(dir, "src", "hookprobe.cppm"), string.format([[
export module hookprobe;
export const char* hook_compiler()  { return "%s"; }
export const char* hook_stdlib()    { return "%s"; }
export const char* hook_target_os() { return "%s"; }
export const char* hook_target()    { return "%s"; }
]], value("MCPP_COMPILER"), value("MCPP_CXX_STDLIB"), value("MCPP_TARGET_OS"),
    value("MCPP_TARGET")))
    return true
end
LUA

cat > src/main.cpp <<'CPP'
#include <cstdio>
import hookprobe;
int main() {
    std::printf("compiler=%s stdlib=%s os=%s target=%s\n",
                hook_compiler(), hook_stdlib(), hook_target_os(), hook_target());
    return 0;
}
CPP

cat > mcpp.toml <<'TOML'
[package]
name    = "consumer"
version = "0.1.0"

[dependencies.acme]
hookprobe = "1.0.0"

[indices]
acme = { path = "local-index" }
TOML

MCPP_COMPILER=inherited MCPP_CXX_STDLIB=inherited "$MCPP" run > run.log 2>&1 \
    || fail "the build or the run failed" run.log
grep -Eq '^compiler= stdlib= os=linux target=[a-z0-9_]+-linux' run.log \
    || fail "the install hook did not see the build's target with empty toolchain names" run.log
echo "install hook environment OK"
