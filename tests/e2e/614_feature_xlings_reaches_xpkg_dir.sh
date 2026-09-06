#!/usr/bin/env bash
# requires: gcc unix-shell
# A tool declared under `[feature-xlings.<f>]` is answerable through
# `mcpp::xpkg_dir` when that feature is active.
#
# THE DEFECT THIS COVERS. `[feature-xlings]` reached the provisioner from the
# start: naming a package there downloaded and installed it. It did not reach
# the build program's environment, which was filled from `[xlings.workspace]`
# alone. So a rule that resolved its compiler through `xpkg_dir` got "" for a
# payload sitting on disk, and the only sensible thing such a program can print
# is "declare this package" -- naming a declaration the author had already
# written. A diagnostic pointing at the wrong file is worse than none.
#
# THE CRITERION HAS A CONTROL. "The path appears" is satisfied by an mcpp that
# answers for every package it has ever installed, so the same project is built
# twice: once with the feature and once without. Only a working gate produces
# both readings -- the path under `--features`, and an empty answer without it.
#
# THE PACKAGE IS ONE mcpp ITSELF BOOTSTRAPS. `xim:ninja` is in every mcpp
# sandbox because mcpp put it there, so this test needs no network and no
# fixture. Its version is READ from the store rather than written here: a
# hardcoded version would turn an mcpp that bumped ninja into a failing test
# about features.
set -e

MCPP="${MCPP:-mcpp}"
MH="${MCPP_HOME:-$HOME/.mcpp}"
store="$MH/registry/data/xpkgs/xim-x-ninja"

if [ ! -d "$store" ]; then
    printf 'SKIP: %s is absent, so there is no installed payload to look up\n' "$store"
    exit 0
fi
# The newest version that actually carries a payload. `ls | head -1` takes
# whichever name sorts first, and a store that has seen two ninja versions can
# hold a directory an uninstall left behind; `xpkg_dir` answers "" for that, and
# the test would then fail about features while really measuring the store.
#
# NON-EMPTY IS THE TEST, NOT `bin/`. Not every payload has a bin directory --
# this one puts the executable at its root -- so a criterion spelling `bin/`
# would skip on a perfectly good payload.
version=""
for candidate in $(ls "$store" | sort -V -r); do
    if [ -n "$(ls -A "$store/$candidate" 2>/dev/null)" ]; then
        version="$candidate"; break
    fi
done
if [ -z "$version" ]; then
    printf 'SKIP: every version under %s is empty\n' "$store"
    exit 0
fi
payload="$store/$version"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/app/src"
cat > "$work/app/mcpp.toml" <<TOML
[package]
name    = "featurexlings"
version = "0.1.0"

[build]
sources = ["src/main.cpp"]

[features]
default = {}
gpu     = {}

[feature-xlings.gpu]
"xim:ninja" = "$version"
TOML
cat > "$work/app/src/main.cpp" <<'CPP'
int main() { return 0; }
CPP
cat > "$work/app/build.mcpp" <<'BUILD'
#include <cstdio>
#include <cstdlib>
int main() {
    const char* dir = std::getenv("MCPP_XPKG_XIM_NINJA_DIR");
    std::printf("mcpp:warning=xpkg_dir=[%s]\n", (dir && *dir) ? dir : "");
    return 0;
}
BUILD

cd "$work/app"

fails=0
check() {
    case "$2" in
        *"$3"*) printf 'ok: %s\n' "$1" ;;
        *) printf 'ASSERT-FAIL: %s -- expected to find %s\n' "$1" "$3"
           fails=$((fails + 1)) ;;
    esac
}

with_gpu="$("$MCPP" build --features gpu 2>&1 || true)"
rm -rf target
without_gpu="$("$MCPP" build 2>&1 || true)"

printf -- '--- with --features gpu ---\n%s\n' "$with_gpu"
printf -- '--- without ---\n%s\n' "$without_gpu"

check "the feature's tool resolves to its payload" \
      "$with_gpu" "xpkg_dir=[$payload]"
# The control, and it is an EQUALITY rather than an absence: "the path is not
# there" is also what a build that never ran the program produces.
check "and answers empty when the feature is off" \
      "$without_gpu" "xpkg_dir=[]"

if [ "$fails" -ne 0 ]; then
    printf 'FAIL: %s assertion(s) failed\n' "$fails"
    exit 1
fi
printf 'PASS: [feature-xlings] reaches xpkg_dir\n'
