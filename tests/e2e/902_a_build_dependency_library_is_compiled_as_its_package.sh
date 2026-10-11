#!/usr/bin/env bash
# requires:
# 902 -- a library in [build-dependencies] is usable from build.mcpp as the
# package it is (mcpp#797, R1).
#
# The shape is a module wrapper over a header library (toml++, nlohmann.json,
# fmt in mcpp-index): the interface unit's global module fragment includes the
# library's own header through the package's `include_dirs`. Before the build
# programs' graph, the interface was compiled alone with the build program's
# flags, and the include failed (`fatal error: hl/hl.hpp: No such file`).
#
#   A  a plain `[build-dependencies]` entry, with no `host-module`, gives the
#      build program the library: `<hl/hl.hpp>` resolves, the package's
#      `defines` apply, and the value reaches the target through a directive;
#   B  `[lib] path` names an interface root outside `src/`, and the module is
#      imported under the name it declares;
#   C  the library is not part of the target (no object of it in the target's
#      plan), since only [build-dependencies] names it.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/hl/include/hl" "$TMP/hl/modules" "$TMP/app/src"
cat > "$TMP/hl/include/hl/hl.hpp" <<'EOF'
#pragma once
namespace hl { inline int answer() { return HL_BASE + 2; } }
EOF
cat > "$TMP/hl/modules/hl.cppm" <<'EOF'
module;
#include <hl/hl.hpp>
export module acme.hl;
export namespace hl { using hl::answer; }
EOF
cat > "$TMP/hl/mcpp.toml" <<'EOF'
[package]
namespace = "acme"
name      = "hl"
version   = "0.1.0"
[lib]
path = "modules/hl.cppm"
[build]
sources      = ["modules/hl.cppm"]
include_dirs = ["include"]
defines      = ["HL_BASE=40"]
[targets.hl]
kind = "lib"
EOF
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[build-dependencies.acme]
hl = { path = "../hl" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
#include <cstdio>
int main() { std::printf("V=%d\n", V); }
EOF
cat > "$TMP/app/build.mcpp" <<'EOF'
import std;
import mcpp;
import acme.hl;
int main() { mcpp::cxxflag(std::format("-DV={}", hl::answer()).c_str()); }
EOF

cd "$TMP/app"
"$MCPP" build > b.log 2>&1 || fail "A: a header-backed module library in [build-dependencies] did not build" b.log
out="$("$MCPP" run 2>/dev/null | tr -d '\r' | grep '^V=' | tail -1)"
[ "$out" = "V=42" ] || fail "A/B: the library's value did not reach the target: $out" b.log
grep -q "host module" b.log && fail "A: the old host-module path was reported" b.log
# C: the target's own plan compiles no unit of the library.
if find target -path '*/obj/*' -name 'hl.m.o*' -not -path '*/.build-mcpp/*' | grep -q .; then
    fail "C: the build dependency was compiled into the target's plan"
fi
echo "PASS: 902_a_build_dependency_library_is_compiled_as_its_package"
