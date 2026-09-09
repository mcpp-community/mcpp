#!/usr/bin/env bash
# requires: gcc
# A PREDICATE CARRYING ONLY A RUNTIME TABLE IS STILL APPLIED.
#
# `[target.<pred>.runtime]` is the dialect-neutral half of a link line: mcpp
# renders `libraries` as `-lm` or `m.lib` depending on the target, which is what
# lets one manifest name a system library for a `cl.exe` consumer and a GNU one
# alike. It has been implemented since 2026.8.29.1, and it worked -- unless it
# was the only thing written under its predicate.
#
# The gate deciding whether to keep a parsed conditional block was a hand-written
# disjunction over the fields its author knew, and `libraries` /
# `link_library_dirs` were added to the struct without being added to it. So the
# block was parsed, populated and dropped.
#
# THE TWO LEGS DIFFER IN ONE DIMENSION THAT HAS NOTHING TO DO WITH LIBRARIES,
# and that is what makes this evidence about the gate rather than about the
# predicate. The second project adds a `defines` entry -- a key the old gate did
# list -- and before the fix that single unrelated line decided whether the
# libraries were applied.
#
# It is also why no existing test caught it: the only fixture exercising the
# neutral form is a GENERATED distribution package, and those always carry
# `ldflags` as well, so the gate always passed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

make_project() {   # $1 = dir, $2 = extra manifest text
    mkdir -p "$1/src"
    cat > "$1/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
    { cat <<'EOF'
[package]
name    = "condrt"
version = "0.1.0"

[target.linux.runtime]
libraries = ["m"]
EOF
      printf '%s\n' "$2"; } > "$1/mcpp.toml"
}

# Leg A: the runtime table is the ONLY thing under the predicate.
make_project alone ""
# Leg B: the control -- identical, plus one unrelated key the old gate listed.
make_project sibling '[target.linux.build]
defines = ["UNRELATED_TO_LIBRARIES"]'

for leg in alone sibling; do
    ( cd "$leg" && "${MCPP:-mcpp}" build > build.log 2>&1 ) || {
        echo "FAIL [$leg]: build"
        cat "$leg/build.log"
        exit 1; }
    graph=$(find "$leg/target" -name build.ninja | head -1)
    [ -n "$graph" ] || { echo "FAIL [$leg]: no build graph"; exit 1; }
    if ! grep -q -- '-lm\b' "$graph"; then
        echo "FAIL [$leg]: [target.linux.runtime] libraries did not reach the link line"
        if [ "$leg" = alone ]; then
            echo "      the block was parsed and then discarded because nothing"
            echo "      else was written under the same predicate"
        fi
        grep -n 'ldflags' "$graph" | head -3
        exit 1
    fi
    echo "ok [$leg]: -lm present"
done

# The reverse leg. A predicate that does NOT match this host must contribute
# nothing, or the two assertions above would also be satisfied by an mcpp that
# stopped evaluating predicates at all.
mkdir -p offhost/src
cat > offhost/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > offhost/mcpp.toml <<'EOF'
[package]
name    = "condrt"
version = "0.1.0"

[target.windows.runtime]
libraries = ["user32"]
EOF
( cd offhost && "${MCPP:-mcpp}" build > build.log 2>&1 ) || {
    echo "FAIL [offhost]: build"; cat offhost/build.log; exit 1; }
graph=$(find offhost/target -name build.ninja | head -1)
grep -q -- '-luser32' "$graph" && {
    echo "FAIL [offhost]: a windows predicate contributed to a linux link line"
    exit 1; }
echo "ok [offhost]: a non-matching predicate contributes nothing"

# An unsupported key in either runtime table is REPORTED, not dropped -- the rule
# [build] and [target.<triple>] have each followed since #418 and #249.
mkdir -p typo/src
cat > typo/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > typo/mcpp.toml <<'EOF'
[package]
name    = "condrt"
version = "0.1.0"

[runtime]
dlopen_lib = ["one"]

[target.linux.runtime]
libraries = ["m"]
framework = ["Cocoa"]
EOF
( cd typo && "${MCPP:-mcpp}" build > build.log 2>&1 ) || {
    echo "FAIL [typo]: build"; cat typo/build.log; exit 1; }
grep -q "unsupported key 'dlopen_lib'" typo/build.log || {
    echo "FAIL [typo]: a plausible typo in [runtime] was accepted in silence"
    cat typo/build.log
    exit 1; }
grep -q "unsupported key 'framework'" typo/build.log || {
    echo "FAIL [typo]: a plausible typo in [target.<pred>.runtime] was accepted in silence"
    cat typo/build.log
    exit 1; }
# The correctly spelled sibling in the same table still took effect, so the
# sweep is not simply reporting everything.
graph=$(find typo/target -name build.ninja | head -1)
grep -q -- '-lm\b' "$graph" || {
    echo "FAIL [typo]: the correctly spelled key stopped working"
    exit 1; }
echo "ok [typo]: reported, and the neighbouring key still applies"
