#!/usr/bin/env bash
# requires: unix-shell
# 1007 -- a requirement stated in a table whose selector names a layer
# (`[target.'cfg(compiler = "llvm")'] requires = [...]`) is checked once the
# layers are resolved, as a requirement of `[package]` is.
#
# openkal-musl links its C library with `-nostdlib`, and on PE under clang the
# routines a compiler emits calls to are compiler-rt's, which the graph supplies
# (openkal-llvm-runtime). It states that as a requirement of the clang build
# only; the table's requirement was dropped without a word, and a graph without
# the layer reached the link with `___chkstk_ms` undefined.
#
# Criteria: A a dependency whose layer-selected table requires a layer this
# build does not resolve is refused before anything is compiled, the message
# naming the dependency, the layer and what it requires; B the same table under
# a selector this build does not match states nothing, and the build succeeds.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

needs() {  # needs <selector>
    mkdir -p "$work/needs/src"
    cat > "$work/needs/mcpp.toml" <<TOML
[package]
name    = "needs"
version = "0.1.0"

[target.'$1']
requires = ["mcpp:compiler-runtime=nonesuch"]

[targets.needs]
kind = "lib"
TOML
    printf 'int needs() { return 7; }\n' > "$work/needs/src/needs.cpp"
}

mkdir -p "$work/app/src"
cat > "$work/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[dependencies]
needs = { path = "../needs" }
TOML
printf 'int needs();\nint main() { return needs() == 7 ? 0 : 1; }\n' > "$work/app/src/main.cpp"

# A: a selector naming a layer, which every build matches.
needs 'cfg(not(compiler = "nonesuch"))'
if (cd "$work/app" && "$MCPP" build > "$work/a.log" 2>&1); then
    cat "$work/a.log"; echo "FAIL: A: a requirement a layer decides was not checked"; exit 1
fi
grep -q 'needs@0.1.0` requires the compiler-runtime to be `nonesuch`' "$work/a.log" \
    || { cat "$work/a.log"; echo "FAIL: A: the refusal does not name the dependency, the layer and what it requires"; exit 1; }
if grep -q 'Compiling' "$work/a.log"; then
    cat "$work/a.log"; echo "FAIL: A: something was compiled before the refusal"; exit 1
fi
echo "ok: A: a requirement a layer decides is checked when the layers are known"

# B: the same table under a selector this build does not match.
needs 'cfg(compiler = "nonesuch")'
rm -rf "$work/app/target"
(cd "$work/app" && "$MCPP" build > "$work/b.log" 2>&1) \
    || { cat "$work/b.log"; echo "FAIL: B: a table this build does not match stated a requirement"; exit 1; }
echo "ok: B: a table this build does not match states nothing"
