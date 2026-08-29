#!/usr/bin/env bash
# requires: gcc
# 309_host_module_identity.sh — a build rule's MODULE NAME is what its source
# declares, not what its package is called.
#
# The codebase already states the general rule for ordinary packages ("module
# names are authored API and are not required to mirror package identity"), and
# the host-module path was the one place that derived the name from
# `package.name` instead of reading it.
#
# That was not merely inconsistent. `build_host_module` binds the name
# differently per compiler family: MSVC is handed `/reference <name>=<ifc>` and
# Clang `-fmodule-file=<name>=<pcm>`, while GCC names nothing at all because its
# BMIs are implicit under gcm.cache, keyed by the name the SOURCE declares. So a
# rule whose lib root declared anything other than its package name built under
# GCC and failed under the other two, reporting a module nobody had written.
#
# Pinned here:
#   1. a divergent declared name is what the consumer imports;
#   2. a dotted name survives whole (`a.b.c`, not `a`);
#   3. two rules declaring ONE module name are refused, naming both packages —
#      before this, the second overwrote the first's object file in silence;
#   4. an `mcpp.` module name from an outside namespace WARNS and still builds.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── 1. the declared name diverges from the package name ─────────────────────
mkdir -p rules/src
cat > rules/mcpp.toml <<'EOF'
[package]
name    = "protobufgen"
version = "0.1.0"

[targets.protobufgen]
kind = "lib"
EOF
# Package `protobufgen`, module `acme.rules.protobuf`. Under the old rule mcpp
# would have registered `protobufgen`, and the consumer's `import` would have
# failed to find it on Clang and MSVC.
cat > rules/src/protobufgen.cppm <<'EOF'
export module acme.rules.protobuf;
import std;
import mcpp;
export namespace acme::rules::protobuf {
inline void apply() { mcpp::cxxflag("-DDIVERGENT_NAME_REACHED=1"); }
}
EOF

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
protobufgen = { path = "../rules", host-module = true }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
#ifndef DIVERGENT_NAME_REACHED
#error "the rule's directive never reached the compile"
#endif
int main() { std::printf("DIVERGENT=%d\n", DIVERGENT_NAME_REACHED); }
EOF
cat > app/build.mcpp <<'EOF'
import mcpp;
import acme.rules.protobuf;   // the DECLARED name, not the package name
int main() { acme::rules::protobuf::apply(); }
EOF

cd app
"$MCPP" build > b1.log 2>&1 || {
    cat b1.log
    echo "FAIL: a rule whose declared module name differs from its package name did not build"
    exit 1; }
out="$("$MCPP" run 2>&1 | grep '^DIVERGENT=' | tail -1)"
[[ "$out" == "DIVERGENT=1" ]] || {
    echo "FAIL: the divergently-named rule's directive did not land: $out"; exit 1; }

# ⚠️ THE IMPORT ABOVE DOES NOT DISCRIMINATE ON GCC, AND THAT IS THE WHOLE
# POINT OF THIS FEATURE. Measured: with the pre-I1 code — which registered the
# PACKAGE name — this build still succeeded under GCC, because GCC's BMIs are
# implicit under gcm.cache and keyed by the name the SOURCE declares, so
# nothing consulted the registered name at all. Under Clang and MSVC the same
# fixture fails, since both are handed an explicit `<name>=<bmi>` mapping.
#
# So the criterion below is the one that works everywhere: `build_host_module`
# derives the BMI and OBJECT filenames from the REGISTERED name. Under the old
# rule the object was `protobufgen.o`; under this one it is named for what the
# source declares. A file listing is platform-independent evidence about which
# name the engine used, where the successful import is not.
BM=target/.build-mcpp
ls "$BM"/acme.rules.protobuf.* >/dev/null 2>&1 || {
    echo "FAIL: the host module was not named for its DECLARED module name"
    ls -la "$BM" 2>/dev/null
    exit 1; }
if ls "$BM"/protobufgen.* >/dev/null 2>&1; then
    ls -la "$BM"
    echo "FAIL: the host module is still named for the PACKAGE name"
    exit 1
fi

# ── 2. two rules, one module name ───────────────────────────────────────────
# Two DIFFERENT packages declaring ONE module name. This is the risk that
# decoupling the module name from the package name introduces, which is why I1
# and this check ship together rather than in sequence.
#
# Measured with the check removed and the rest of this change in place: both
# compiles write `sharedname.o`, the second overwrites the first, and the one
# surviving object is handed to the link twice — once per registered module.
# The build fails, but it fails as
#
#     ld: …/sharedname.o: multiple definition of `initializer for module
#     sharedname'; …/sharedname.o: beta.cppm: first defined here
#
# naming one file twice, one package twice, and never saying that two packages
# are involved or which two. The overwrite is the silent part; the diagnostic
# is merely unactionable. This check is what turns it into a sentence.
cd "$TMP"
for n in alpha beta; do
    mkdir -p "dup-$n/src"
    cat > "dup-$n/mcpp.toml" <<EOF
[package]
name      = "$n"
namespace = "vendor$n"
version   = "0.1.0"

[targets.$n]
kind = "lib"
EOF
    cat > "dup-$n/src/$n.cppm" <<EOF
export module sharedname;
import std;
import mcpp;
export namespace shared { inline void go() { mcpp::cxxflag("-DFROM_$n=1"); } }
EOF
done
mkdir -p dupapp/src
cat > dupapp/mcpp.toml <<'EOF'
[package]
name    = "dupapp"
version = "0.1.0"

[dependencies]
alpha = { path = "../dup-alpha", host-module = true }
beta  = { path = "../dup-beta",  host-module = true }
EOF
echo 'int main() { return 0; }' > dupapp/src/main.cpp
cat > dupapp/build.mcpp <<'EOF'
import mcpp;
import sharedname;
int main() { shared::go(); }
EOF
cd dupapp
if "$MCPP" build > b2.log 2>&1; then
    cat b2.log
    echo "FAIL: two rules declaring the same module name were accepted"
    exit 1
fi
# The two package identities and the two interface paths ARE the content of
# this diagnostic: with one module name shared they are the only way to tell
# the packages apart. Asserted on the whole output, not on a keyword, so a
# reworded message that dropped them would fail here.
for want in "sharedname" "vendoralpha.alpha" "vendorbeta.beta" \
            "dup-alpha/src/alpha.cppm" "dup-beta/src/beta.cppm"; do
    grep -qF "$want" b2.log || {
        cat b2.log
        echo "FAIL: the collision diagnostic does not name '$want'"; exit 1; }
done

# ── 3. the reserved `mcpp.` prefix warns, and the build still succeeds ───────
# Both halves. A test that only checked for the warning could not tell a
# warning from an error.
cd "$TMP"
mkdir -p claimer/src
cat > claimer/mcpp.toml <<'EOF'
[package]
name      = "claimer"
namespace = "acme"
version   = "0.1.0"

[targets.claimer]
kind = "lib"
EOF
cat > claimer/src/claimer.cppm <<'EOF'
export module mcpp.build.claimed;
import std;
import mcpp;
export namespace claimed { inline void go() { mcpp::cxxflag("-DCLAIMED=1"); } }
EOF
mkdir -p claimapp/src
cat > claimapp/mcpp.toml <<'EOF'
[package]
name    = "claimapp"
version = "0.1.0"

[dependencies]
claimer = { path = "../claimer", host-module = true }
EOF
cat > claimapp/src/main.cpp <<'EOF'
#include <cstdio>
#ifndef CLAIMED
#error "the rule never ran"
#endif
int main() { std::printf("CLAIMED=%d\n", CLAIMED); }
EOF
cat > claimapp/build.mcpp <<'EOF'
import mcpp;
import mcpp.build.claimed;
int main() { claimed::go(); }
EOF
cd claimapp
"$MCPP" build > b3.log 2>&1 || {
    cat b3.log
    echo "FAIL: the reserved prefix must WARN, not fail the build"; exit 1; }
grep -qF "mcpp.build.claimed" b3.log || {
    cat b3.log; echo "FAIL: no warning naming the claimed module"; exit 1; }
grep -qF "acme.claimer" b3.log || {
    cat b3.log; echo "FAIL: the warning does not name the package making the claim"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^CLAIMED=' | tail -1)"
[[ "$out" == "CLAIMED=1" ]] || {
    echo "FAIL: the warned-about rule did not actually work: $out"; exit 1; }

# A package IN the mcpp namespace says the same thing without a warning.
cd "$TMP"
sed -i 's/^namespace = "acme"/namespace = "mcpp"/' claimer/mcpp.toml
cd claimapp && rm -rf target
"$MCPP" build > b4.log 2>&1 || {
    cat b4.log; echo "FAIL: the official namespace build failed"; exit 1; }
if grep -qF "reserved for rules maintained by the mcpp project" b4.log; then
    cat b4.log
    echo "FAIL: a package in the mcpp namespace was warned about its own prefix"
    exit 1
fi

echo "OK"
