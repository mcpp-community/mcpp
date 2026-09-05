#!/usr/bin/env bash
# An exclusive capability with two providers is refused at binding time.
#
# Two packages providing one capability is usually fine and sometimes the
# point: an OpenBLAS package and an MKL package both provide `blas`, and a
# build that links one program against each is legitimate. What is not fine is
# two implementations of ONE accelerator interface in one link: they define the
# same symbols, and the link resolves every call to whichever archive it
# reached first.
#
# The engine cannot tell those apart -- seeing the symbol overlap needs object
# files that do not exist when capabilities are bound -- so the package says it.
# This asserts both halves: the declared-exclusive pair is refused, and the pair
# that declares nothing still builds.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkpkg() {                        # $1 name  $2 capability  $3 "exclusive"|""
    mkdir -p "$1/src"
    {
        echo '[package]'
        echo "name    = \"$1\""
        echo 'version = "0.1.0"'
        echo "provides = [\"$2\"]"
        [ -n "$3" ] && echo "exclusive = [\"$2\"]"
        echo '[language]'
        echo 'standard = "c++23"'
        echo '[targets.'"$1"']'
        echo 'kind = "lib"'
    } > "$1/mcpp.toml"
    echo "int ${1//-/_}_anchor(void) { return 0; }" > "$1/src/$1.c"
}

consumer() {                     # $@ dep names
    rm -rf app; "$MCPP" new app > /dev/null; cd app
    {
        echo '[package]'
        echo 'name    = "app"'
        echo 'version = "0.1.0"'
        echo '[language]'
        echo 'standard = "c++23"'
        echo '[dependencies]'
        for d in "$@"; do echo "$d = { path = \"../$d\" }"; done
    } > mcpp.toml
    rm -f src/*.cppm
    echo 'int main(){return 0;}' > src/main.cpp
}

# ── Half one: two exclusive providers of one capability are refused ─────────
mkpkg gpublas-a gpu-blas exclusive
mkpkg gpublas-b gpu-blas exclusive
consumer gpublas-a gpublas-b

if "$MCPP" build > out.log 2>&1; then
    cat out.log
    echo "FAIL: two exclusive providers of 'gpu-blas' were accepted"
    exit 1
fi
grep -q "gpu-blas"   out.log || { cat out.log; echo "FAIL: refusal does not name the capability"; exit 1; }
grep -q "gpublas-a"  out.log || { cat out.log; echo "FAIL: refusal does not name the first provider"; exit 1; }
grep -q "gpublas-b"  out.log || { cat out.log; echo "FAIL: refusal does not name the second provider"; exit 1; }
grep -qi "exclusive" out.log || { cat out.log; echo "FAIL: refusal does not say why"; exit 1; }
echo "PASS: exclusive pair refused, naming the capability and both providers"

# ── Half two: the SAME graph without the claim still builds ────────────────
#
# The control matters. Without it this test would also pass if mcpp refused
# every duplicate provider, which is the behaviour the `exclusive` key exists
# to avoid: it would break the documented OpenBLAS/MKL case.
cd "$TMP"
mkpkg blas-a blas
mkpkg blas-b blas
consumer blas-a blas-b

"$MCPP" build > ok.log 2>&1 || { cat ok.log; echo "FAIL: two ordinary providers were refused"; exit 1; }
echo "PASS: two ordinary providers of one capability coexist"

echo "PASS: exclusive capability"
