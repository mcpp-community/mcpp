#!/usr/bin/env bash
# requires: elf gcc
# A DEPENDENCY's `[feature-xlings.<f>]` must be installed before that
# dependency's build.mcpp runs.
#
# THE ROOT AND A DEPENDENCY WERE NOT THE SAME PATH. The root's declarations are
# provisioned early; the graph's were provisioned ~1700 lines further down, after
# every build.mcpp had already run. A package that declares a tool under the
# feature that needs it and then asks for it with `xpkg_dir` therefore worked as
# the root and was refused as a dependency -- with the very declaration it had
# already made quoted back at it.
#
# Measured on the published `ggml-org:llamacpp@b10069.2`: a clean MCPP_HOME
# pulled twenty-four xim payloads for that graph and not `xim:shaderc`.
#
# WHY IT SURVIVED EVERY OTHER CHECK. Once the tool is in the registry for any
# reason -- and building the package itself puts it there -- `xpkg_dir` finds it
# and the ordering stops mattering. The package's own CI builds it as the ROOT.
# Only a registry that has never seen it can tell, which is why this test uses
# an isolated MCPP_HOME rather than the ambient one.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# THE TOOL MUST BE ONE mcpp DOES NOT INSTALL FOR ITS OWN REASONS.
#
# The first version of this test used `xim:ninja`, and it passed on the broken
# engine: mcpp installs ninja to run its own builds, so `xpkg_dir` answered
# whether or not the feature had been provisioned. The criterion had selected an
# object that is present for an unrelated reason -- it could never have failed.
#
# Measured: a minimal isolated MCPP_HOME holds binutils, gcc, gcc-runtime,
# gcc-specs-config, glibc, linux-headers, ninja and patchelf. `shaderc` is in
# none of that, is 16M, and is the tool the real defect was found with.
TOOL=shaderc
TOOL_VERSION="2026.3"

mkdir -p dep/src
cat > dep/src/lib.cpp <<'EOF'
int dep_touch() { return 1; }
EOF
cat > dep/build.mcpp <<EOF
#include <cstdio>
#include <string>
import mcpp;
int main() {
    // THE ASSERTION IS HERE, in the dependency's own build program: the tool it
    // declared under an active feature has to be findable by the time it runs.
    const char* d = mcpp::xpkg_dir("xim", "$TOOL");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr,
            "a dependency's [feature-xlings] tool was not provisioned before "
            "its build.mcpp ran\n");
        return 2;
    }
    // TO A FILE, NOT TO stdout. mcpp prints a build program's output only when
    // it FAILS, so an assertion that grepped the build log for this line would
    // be unreachable on exactly the run that is supposed to produce it.
    std::string out = std::string(mcpp::manifest_dir()) + "/tool-seen.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d);
    std::fclose(f);
    return 0;
}
EOF
cat > dep/mcpp.toml <<EOF
[package]
name    = "featuretool"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[features]
default   = []
usestool  = []
[feature-xlings.usestool]
"xim:$TOOL" = "$TOOL_VERSION"
[targets.featuretool]
kind = "lib"
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[dependencies]
featuretool = { path = "../dep", features = ["usestool"] }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

# AN ISOLATED HOME IS THE POINT. The ambient registry very likely holds the
# tool already, and then this test passes on a broken engine.
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"

cd app
if ! "$MCPP" build >build.log 2>&1; then
    echo "FAIL: the consumer did not build"
    grep -iE 'not provisioned|error' build.log | head -5
    exit 1
fi
seen="$TMP/dep/tool-seen.txt"
[ -s "$seen" ] || {
    echo "FAIL: the dependency's build program left no record of finding its tool"
    tail -10 build.log
    exit 1
}
echo "the dependency's build program saw its tool at: $(cat "$seen")"
grep -q "xim-x-$TOOL" "$seen" || {
    echo "FAIL: the recorded path does not name $TOOL: $(cat "$seen")"
    exit 1
}
echo "PASS: a dependency's [feature-xlings] tool is provisioned before its build.mcpp runs"
