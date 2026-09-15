#!/usr/bin/env bash
# requires: llvm elf
# 700 -- one process, one C++ runtime (#646 F3a), read with llvm@22.1.8.
#
# The ELF defaults gave a program `self-contained` and a shared library
# `toolchain-coupled`. A program that loads a C++ shared library therefore held
# a static libc++ and `libc++.so.1`: the executable exported the runtime symbols
# the library referenced, the library bound some of them there and kept the
# rest, and the program aborted the first time the library formatted a string:
#
#   libc++abi: terminating due to uncaught exception of type std::bad_cast
#
# (measured on 2026.9.15.2, exit 134, the build itself green). An unstated
# program contract now takes the shared library's when the program loads a C++
# shared library of the build. Three legs: the default shape runs, with the
# program coupled to the same runtime and the record saying so; `--strict`
# passes, so the symbol-provision check no longer reports the module
# initialiser both images link; a stated self-contained program over the
# coupled library is refused before compiling, with its reason.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

READELF=$(command -v readelf || true)
[ -n "$READELF" ] || READELF=$(ls "$MCPP_HOME"/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/llvm-readelf 2>/dev/null | head -1)
[ -n "$READELF" ] || fail "no readelf and no llvm-readelf to read NEEDED with"

cd "$TMP"
mkdir -p lib/src app/src
cat > lib/mcpp.toml <<'TOML'
[package]
name    = "lib"
version = "0.1.0"

[targets.lib]
kind = "shared"
TOML
cat > lib/src/lib.cppm <<'CPP'
export module lib;
import std;
export [[gnu::visibility("default")]] std::string lib_greet(int n);
CPP
cat > lib/src/lib.cpp <<'CPP'
module lib;
import std;
std::string lib_greet(int n) { return std::format("lib-{}", n); }
CPP

write_app() {   # $1 = extra [build] lines
    cat > app/mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[build]
$1

[dependencies]
lib = { path = "../lib" }
TOML
}
cat > app/src/main.cpp <<'CPP'
import std;
import lib;
int main() { std::println("{}", lib_greet(3)); }
CPP

# ── The default shape runs, coupled to one runtime ──────────────────────────
write_app ''
cd app
"$MCPP" build --strict > default.log 2>&1 \
    || fail "the default shape did not build under --strict" default.log
dir=$(ls -d target/*/*/ | head -1)
out=$("$dir/bin/app" 2>&1) || fail "the program aborted: $out" default.log
[ "$out" = "lib-3" ] || fail "expected lib-3, got: $out"
"$READELF" -d "$dir/bin/app" | grep -q 'Shared library: \[libc++.so.1\]' \
    || fail "the program does not load libc++.so.1, so it carries its own runtime" default.log
grep -q '"distributable": *"toolchain-coupled"' "$dir/resolution.json" \
    || fail "resolution.json does not record the program as toolchain-coupled" "$dir/resolution.json"
grep -q 'also provided by a library it loads' default.log \
    && fail "the symbol-provision check reported the shared shape" default.log
echo "ok: a program over a C++ shared library runs on one C++ runtime"
cd ..

# ── A stated self-contained program over the coupled library is refused ─────
write_app 'cxx_runtime = { default = "self-contained", shared = "toolchain-coupled" }'
cd app
rm -rf target
if "$MCPP" build > split.log 2>&1; then
    fail "a stated self-contained program over a coupled C++ shared library was not refused" split.log
fi
grep -q "state a self-contained C++ runtime and load the C++ shared library 'lib'" split.log \
    || fail "the refusal does not name the statement and the library" split.log
grep -q 'cxx_runtime = { shared = "self-contained" }' split.log \
    || fail "the refusal does not name the private-copy remedy" split.log
ninja=$(ls target/*/*/build.ninja 2>/dev/null | head -1 || true)
[ -z "$ninja" ] || ! grep -q '^build bin/liblib.so' "$ninja" \
    || fail "a build graph was written before the refusal" "$ninja"
if command -v jq >/dev/null 2>&1; then
    "$MCPP" why toolchain --format json > why.json 2>/dev/null || true
    reason=$(jq -r '.data.reason // empty' why.json 2>/dev/null || true)
    [ "$reason" = "program-cxx-runtime-split" ] \
        || fail "the machine reason was '$reason', expected program-cxx-runtime-split" why.json
fi
echo "ok: a stated self-contained program over a coupled C++ shared library is refused"

echo "PASS: 700 a program over a C++ shared library has one C++ runtime"
