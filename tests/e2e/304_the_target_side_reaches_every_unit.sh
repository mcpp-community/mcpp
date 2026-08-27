#!/usr/bin/env bash
# requires: unix-shell jq
# A layer the GRAPH supplies is beneath every unit, including a sibling
# dependency package that has never heard of it.
#
# ⭐⭐ THE SET WAS ALREADY COMPUTED AND REACHED ONE TRANSLATION UNIT.
#
# A package that supplies a target-side layer publishes the headers the whole
# target is built against. Those travelled as an ordinary `publicUsage`, which
# propagates ALONG DEPENDENCY EDGES — so the root and the provider's own units
# received them and a SIBLING dependency package did not. `nlohmann.json` is
# not downstream of `openkal-llvm-runtime`; it is beside it.
#
# The result is two flavours of BMI in one build: `std` compiled over the
# target's libc++ (correct — prepare hands it exactly this set) and the
# dependency packages compiled over the payload's. Any unit importing both
# fails at the first template instantiation touching a declaration present in
# both header sets:
#
#     istream:1245: error: reference to 'space' is ambiguous
#
# mcpp#514 §A.
#
# ⚠️ NO openkal, NO CROSS, NO NETWORK. A path package declaring one capability
# and one `include_dirs` entry reproduces it, which is the point: this is not a
# property of openkal but of how a target side was modelled.
#
# ⭐ THE CRITERION IS THE CDB ROW, NOT A GREP OVER THE TREE. `compile_commands.json`
# says what each unit is actually compiled with; a grep over `build.ninja` would
# also match the global rule and pass for the wrong reason.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p src abi/src abi/abi-include abi/abi-internal dep/src dep/dep-include

# The provider: supplies a target-side layer, publishes one directory, and
# keeps one to itself.
#
# ⭐⭐ THE TWO KEYS OF THIS RELEASE MEET IN THIS ONE PACKAGE, AND THAT IS THE
# REAL-WORLD SHAPE. `openkal-musl` declares `provides = ["mcpp:c-abi=musl"]`
# AND carries an internal header overlay. A layer's directories reach EVERY
# unit, so publishing the overlay by mistake would put musl's `hidden`/`weak`
# macros not merely on its consumers but on every translation unit of the
# build — a strictly wider blast radius than the leak `private_include_dirs`
# was written for.
#
# The two compose correctly by construction (the target side is assembled from
# the provider's `publicUsage`, which is the filtered set), and "by
# construction" is what an assertion is for.
cat > abi/mcpp.toml <<'EOF'
[package]
name    = "abiprov"
version = "0.1.0"
provides = ["mcpp:c++-abi=libc++"]

[build]
include_dirs         = ["abi-internal", "abi-include"]
private_include_dirs = ["abi-internal"]
EOF
printf 'export module abiprov;\nexport int abiprov_v() { return 1; }\n' > abi/src/abiprov.cppm
printf '#pragma once\n' > abi/abi-include/marker.h
printf '#pragma once\n' > abi/abi-internal/private.h

# The sibling: a dependency package that does NOT depend on the provider.
cat > dep/mcpp.toml <<'EOF'
[package]
name    = "sibling"
version = "0.1.0"

[build]
include_dirs = ["dep-include"]
EOF
printf 'export module sibling;\nexport int sibling_v() { return 2; }\n' > dep/src/sibling.cppm
printf '#pragma once\n' > dep/dep-include/own.h

cat > mcpp.toml <<'EOF'
[package]
name    = "reachprobe"
version = "0.1.0"

[dependencies]
abiprov = { path = "abi" }
sibling = { path = "dep" }
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
import abiprov;
import sibling;
int main() { std::printf("%d\n", abiprov_v() + sibling_v()); }
EOF

"$MCPP" build >/dev/null 2>&1 || {
    echo "FAIL: the probe project did not build"
    "$MCPP" build 2>&1 | tail -20 | sed 's/^/        /'
    exit 1
}

[ -s compile_commands.json ] || { echo "FAIL: no compile_commands.json"; exit 1; }

# ⚠️ A DENOMINATOR. A CDB with no sibling row would make every assertion below
# vacuously true, which is the false green this criterion has to rule out.
siblings="$(jq -r '[.[] | select(.file | test("/dep/src/"))] | length' compile_commands.json)"
if [ "$siblings" -lt 1 ]; then
    echo "FAIL: the CDB has no sibling-package row — nothing was checked"
    exit 1
fi

missing="$(jq -r '
  [ .[]
    | select(.file | test("/dep/src/"))
    | { f: .file, has: ((.arguments // (.command | split(" "))) | map(test("abi-include")) | any) }
    | select(.has | not)
    | .f
  ] | .[]' compile_commands.json)"

if [ -n "$missing" ]; then
    echo "FAIL: the target side did not reach $siblings sibling unit(s):"
    printf '%s\n' "$missing" | sed 's/^/        /'
    exit 1
fi

# And the reverse direction: a package that supplies NO layer must not have its
# own directory pushed onto everyone. Otherwise this test would pass for a
# build that simply gives every include dir to every unit.
leaked="$(jq -r '
  [ .[]
    | select(.file | test("/abi/src/"))
    | { f: .file, has: ((.arguments // (.command | split(" "))) | map(test("dep-include")) | any) }
    | select(.has)
    | .f
  ] | .[]' compile_commands.json)"

if [ -n "$leaked" ]; then
    echo "FAIL: a non-layer package's directory reached a sibling — the fix is too wide:"
    printf '%s\n' "$leaked" | sed 's/^/        /'
    exit 1
fi

# ⭐⭐ AND THE LAYER'S *PRIVATE* DIRECTORY REACHES NOBODY BUT ITSELF.
#
# A layer reaches every unit, so publishing an internal overlay by mistake is
# wider here than anywhere else: it would land on every translation unit of the
# build rather than on the provider's consumers.
private_leak="$(jq -r '
  [ .[]
    | select((.file | test("/abi/src/")) | not)
    | { f: .file, has: ((.arguments // (.command | split(" "))) | map(test("abi-internal")) | any) }
    | select(.has)
    | .f
  ] | .[]' compile_commands.json)"

if [ -n "$private_leak" ]; then
    echo "FAIL: the layer's PRIVATE directory reached units outside the package:"
    printf '%s\n' "$private_leak" | sed 's/^/        /'
    exit 1
fi

# The control: private is not the same as dropped. The provider's own units
# must still be built with it, or the assertion above passes for a build that
# simply lost the directory.
own_private="$(jq -r '
  [ .[]
    | select(.file | test("/abi/src/"))
    | select((.arguments // (.command | split(" "))) | map(test("abi-internal")) | any)
  ] | length' compile_commands.json)"
if [ "$own_private" -lt 1 ]; then
    echo "FAIL: the provider lost its own private directory — the check above proves nothing"
    exit 1
fi

echo "OK: the graph-supplied target side reaches every unit ($siblings sibling units), its private directory reaches only its own $own_private unit(s), and nothing else spreads"
