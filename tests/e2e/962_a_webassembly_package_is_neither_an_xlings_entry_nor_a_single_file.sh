#!/usr/bin/env bash
# requires: unix-shell
# 962 -- a WebAssembly package is neither an xlings entry nor a single file
# (SPEC-014 §1.5, §8; design 2026-10-29).
#
# An xlings entry installs a program of the system it runs on, under that
# system's key and for machine architectures; a `wasm32-emscripten` package
# is a module a JavaScript engine runs (`bin/<name>.js`, `bin/<name>.wasm`).
# `mcpp emit xpkg` described one under `linux` with `archs = {"wasm32"}`
# (measured on 2026.10.28.1). A single file is an executable of one system,
# and `--pkg-format single-file` on the wasm row tried a `wasm32-linux-musl`
# launcher. Both are refused, naming the route that serves the web.
#
# Criteria:
#   A. `mcpp emit xpkg --package` of a wasm32-emscripten package exits 2,
#      naming the package, its triple and `dist-web`, and writes no entry.
#   B. Where an emsdk payload is installed, `mcpp pack --target
#      wasm32-emscripten --pkg-format single-file` is refused before anything
#      compiles, naming both choices and the formats that serve the row.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# A -- the package `mcpp pack --target wasm32-emscripten` writes, as its
# mcpp-package.json describes it.
top=web-0.1.0-wasm32-emscripten
mkdir -p "$TMP/$top/bin"
cd "$TMP"
printf 'console.log("web");\n' > "$top/bin/web.js"
printf '\0asm\1\0\0\0' > "$top/bin/web.wasm"
cat > "$top/mcpp-package.json" <<'JSON'
{"schema": 1, "name": "web", "version": "0.1.0", "platform": null, "linkage": "declared",
 "mode": "vendored", "legs": ["wasm32-emscripten"], "systems": [],
 "launch": {"program": "bin/web.js"},
 "files": [{"path": "bin/web.js", "role": "program", "leg": "wasm32-emscripten", "sha256": "0"},
           {"path": "bin/web.wasm", "role": "program", "leg": "wasm32-emscripten", "sha256": "0"}]}
JSON
tar czf "$top.tar.gz" "$top"
rc=0
"$MCPP" emit xpkg --package "$top.tar.gz" --url https://example.org/v0.1.0 > a.lua 2> a.err || rc=$?
[ "$rc" = 2 ] || fail "A: emit xpkg of a wasm package exited $rc, not 2" a.err a.lua
grep -q "$top.tar.gz is a package of wasm32-emscripten, which is no program of Linux, macOS or Windows" a.err \
    || fail "A: the refusal does not name the package and its triple" a.err
grep -q "dist-web" a.err || fail "A: the refusal does not name the route of the web" a.err
[ -s a.lua ] && fail "A: an entry was written" a.lua
echo "ok: emit xpkg refuses a WebAssembly package"

# B
have_emsdk=0
for d in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-emsdk/*/emscripten \
         "$HOME"/.xlings/data/xpkgs/xim-x-emsdk/*/emscripten; do
    [[ -x "$d/em++" ]] && have_emsdk=1
done
if [ "$have_emsdk" -ne 1 ]; then
    echo "  (no emsdk payload installed: B is not run here)"
else
    mkdir -p "$TMP/web/src"
    cd "$TMP/web"
    printf '[package]\nname    = "web"\nversion = "0.1.0"\n' > mcpp.toml
    printf 'import std;\nint main() { std::println("web"); }\n' > src/main.cpp
    rc=0
    "$MCPP" pack --target wasm32-emscripten --pkg-format single-file > b.log 2>&1 || rc=$?
    [ "$rc" = 2 ] || fail "B: single-file of a wasm program exited $rc, not 2" b.log
    grep -q "single-file makes an executable of one system, and wasm32-emscripten builds a WebAssembly module" b.log \
        || fail "B: the refusal does not name both choices" b.log
    grep -q "use: --pkg-format tar | zip | dir" b.log || fail "B: the refusal names no way out" b.log
    grep -q "Compiling" b.log && fail "B: refused after compiling" b.log
    echo "ok: --pkg-format single-file refuses a WebAssembly program before a build"
fi

echo "PASS: 962 a WebAssembly package is neither an xlings entry nor a single file"
