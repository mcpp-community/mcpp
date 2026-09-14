#!/usr/bin/env bash
# requires: elf gcc
# 670 -- #634, item 11 of the triage record. A search path that begins with a
# token the loader expands is relative to a loaded object, not to the package
# that wrote it, so `mcpp` leaves it as written: ELF's `$ORIGIN` always was,
# and Mach-O's `@executable_path`, `@loader_path` and `@rpath` were anchored to
# the package directory instead -- `-Wl,-rpath,@executable_path/../Frameworks`
# reached the binary as `<package dir>/@executable_path/../Frameworks`, which
# is how a bundle's framework rpath could not load (measured on macos-15).
#
# The normalisation is host-independent, so Linux measures it: the root's
# `[build] ldflags` and a dependency's, which reach the consumer through a
# second copy of the normaliser. The negative direction is an ordinary
# relative rpath, which is still anchored to the package that wrote it.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p dep/src app/src
cat > dep/mcpp.toml <<'EOF'
[package]
namespace = "demo"
name      = "dep"
version   = "0.1.0"
[targets.dep]
kind = "lib"
[build]
ldflags = ["-Wl,-rpath,@loader_path/../lib"]
EOF
printf 'export module dep;\nexport int dep_anchor() { return 3; }\n' > dep/src/dep.cppm

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[toolchain]
linux = "gcc@16.1.0"
[dependencies]
demo.dep = { path = "../dep" }
[build]
ldflags = ["-Wl,-rpath,@executable_path/../Frameworks", "-Wl,-rpath,vendor/lib"]
EOF
printf 'import dep;\nint main() { return dep_anchor() == 3 ? 0 : 1; }\n' > app/src/main.cpp
cd app

"$MCPP" build > build.log 2>&1 || fail "mcpp build failed" build.log
bin=$(find target -path '*/bin/app' -type f | head -1)
[ -n "$bin" ] || fail "no bin/app" build.log
rpath=$(readelf -d "$bin" | grep -E 'RPATH|RUNPATH' | sed 's/.*\[\(.*\)\]/\1/')
[ -n "$rpath" ] || fail "the program carries no RPATH" build.log

tr ':' '\n' <<<"$rpath" > entries.txt
grep -qx '@executable_path/../Frameworks' entries.txt \
    || fail "the root's @executable_path rpath is not literal" entries.txt
grep -qx '@loader_path/../lib' entries.txt \
    || fail "the dependency's @loader_path rpath is not literal" entries.txt
if grep -q '/@executable_path\|/@loader_path' entries.txt; then
    fail "a loader token was anchored to a package directory" entries.txt
fi
echo "  ok: @executable_path and @loader_path reach the binary as written"

grep -qx "$(pwd -P)/vendor/lib" entries.txt || grep -qx "$(pwd)/vendor/lib" entries.txt \
    || fail "an ordinary relative rpath is no longer anchored to the package" entries.txt
echo "  ok: an ordinary relative rpath is still anchored to the package directory"

echo "670: an rpath loader token is left as written OK"
