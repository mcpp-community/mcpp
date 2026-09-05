#!/usr/bin/env bash
# A package needing more of the machine than it has is refused before compiling.
#
# The failure this prevents is not visible at build time on its own: a program
# built against a runtime newer than the driver it will meet compiles cleanly,
# links cleanly, and fails at first use with a message naming neither side.
# Measured on the development machine, a CUDA 13.3 build against a driver
# serving 12.4 does exactly that.
#
# Nothing here mentions a vendor. The engine reads a name, a relation and a
# version; `cuda.driver` is data passing through. That is asserted directly in
# the third case with a name no backend uses.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A package that states a fact about this machine, the way a package that
# probed at install time would.
mkfact() {                        # $1 name  $2 "cuda.driver=12.4"
    mkdir -p "$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n[language]\nstandard = "c++23"\n[runtime]\nprovides = ["%s"]\n[targets.%s]\nkind = "lib"\n' "$1" "$2" "$1" > "$1/mcpp.toml"
    echo "int ${1//-/_}_anchor(void){return 0;}" > "$1/src/$1.c"
}

# A package that needs something of the machine.
mkneed() {                        # $1 name  $2 "cuda.driver >= 13.0"
    mkdir -p "$1/src"
    {
        printf '[package]\nname = "%s"\nversion = "0.1.0"\n[language]\nstandard = "c++23"\n' "$1"
        printf '[[runtime.requirements]]\nkind = "version-floor"\nvalue = "%s"\n' "$2"
        printf '[targets.%s]\nkind = "lib"\n' "$1"
    } > "$1/mcpp.toml"
    echo "int ${1//-/_}_anchor(void){return 0;}" > "$1/src/$1.c"
}

consumer() {
    rm -rf app; "$MCPP" new app > /dev/null; cd app
    { printf '[package]\nname="app"\nversion="0.1.0"\n[language]\nstandard="c++23"\n[dependencies]\n'
      for d in "$@"; do echo "$d = { path = \"../$d\" }"; done; } > mcpp.toml
    rm -f src/*.cppm
    echo 'int main(){return 0;}' > src/main.cpp
}

# ── One: the floor is above the fact ───────────────────────────────────────
mkfact driverfact "cuda.driver=12.4"
mkneed toolkitnew "cuda.driver >= 13.0"
consumer driverfact toolkitnew

if "$MCPP" build > out.log 2>&1; then
    cat out.log; echo "FAIL: a floor above the stated fact was accepted"; exit 1
fi
grep -q "cuda.driver" out.log || { cat out.log; echo "FAIL: refusal does not name what is short"; exit 1; }
grep -q "13.0"        out.log || { cat out.log; echo "FAIL: refusal does not say what was needed"; exit 1; }
grep -q "12.4"        out.log || { cat out.log; echo "FAIL: refusal does not say what is there"; exit 1; }
grep -q "driverfact"  out.log || { cat out.log; echo "FAIL: refusal does not say who stated the fact"; exit 1; }
echo "PASS: refused, naming the requirement, both versions and who stated the fact"

# ── Two: the floor is met ──────────────────────────────────────────────────
cd "$TMP"
mkneed toolkitok "cuda.driver >= 12.0"
consumer driverfact toolkitok
"$MCPP" build > ok.log 2>&1 || { cat ok.log; echo "FAIL: a met floor was refused"; exit 1; }
echo "PASS: a met floor builds"

# ── Three: a floor with NO fact is silent, and the name is just data ───────
#
# This is the control that matters. Without it the first case would also pass
# against an engine that refused every version-floor requirement, which would
# turn "we do not know" into "no" — the exact failure mode this mechanism was
# built to avoid.
cd "$TMP"
mkneed futureneed "some.future.thing >= 4.2.1"
consumer futureneed
"$MCPP" build > quiet.log 2>&1 || { cat quiet.log; echo "FAIL: a floor nobody answered was refused"; exit 1; }
echo "PASS: a floor with no stated fact is silent, for a name no backend uses"

echo "PASS: version floor"
