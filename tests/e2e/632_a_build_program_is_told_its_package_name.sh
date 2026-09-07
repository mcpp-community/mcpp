#!/usr/bin/env bash
# requires: gcc
# A BUILD PROGRAM IS TOLD WHICH PACKAGE IT IS BUILDING.
#
# Every name a rule package generates is derived from this one: the module a
# consumer imports, the namespace the accessors sit in, the symbols in a
# generated header. Until 2026.9.7.1 nothing answered it, and the closest thing
# available was the leaf of `MCPP_MANIFEST_DIR` -- a DIRECTORY name.
#
# THE DIRECTORY AND THE PACKAGE ARE DELIBERATELY DIFFERENT HERE, and that is the
# whole test. A fixture whose package name happens to equal its directory leaf
# passes against both the old derivation and the new one, so it would assert
# nothing. `mcpp.rules.spirv` shipped with exactly that defect: an example laid
# out as `vulkan/app/` with `name = "vulkan-saxpy"` generated `app.shaders`, and
# every `<something>/app/` in a workspace claimed the same module.
#
# The answer is written to a FILE rather than printed, because mcpp shows a
# build program's stdout only when it exits non-zero -- a criterion reading the
# build log would be measuring the failure path.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# The directory is `app`. The package is not.
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF

cat > app/build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "name=%s\n", mcpp::package_name());
    std::fprintf(f, "namespace=%s\n", mcpp::package_namespace());
    std::fclose(f);
    return 0;
}
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name      = "vulkan-saxpy"
namespace = "example"
version   = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.vulkan-saxpy]
kind = "bin"
main = "src/main.cpp"
EOF

cd app
"${MCPP:-mcpp}" build > build.log 2>&1 || { echo "FAIL: build"; cat build.log; exit 1; }

[ -f answered.txt ] || { echo "FAIL: the build program wrote no answer"; exit 1; }
cat answered.txt

grep -qx 'name=vulkan-saxpy' answered.txt || {
    echo "FAIL: package_name() did not answer the [package] name"
    echo "      (a directory-derived answer would read 'app')"
    exit 1; }
grep -qx 'namespace=example' answered.txt || {
    echo "FAIL: package_namespace() did not answer the [package] namespace"
    exit 1; }

# THE REVERSE LEG: the directory leaf is `app`, so an implementation that still
# derived from the directory would have written `app` above. Assert the two are
# actually different in this fixture, or the check above proves nothing.
[ "$(basename "$PWD")" = "app" ] || {
    echo "FAIL: this fixture no longer distinguishes the two derivations"
    exit 1; }

echo "PASS: a build program reads its package identity, not its directory name"
