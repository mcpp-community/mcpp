#!/usr/bin/env bash
# requires: gcc
# The two rules that govern the target side, each stated as a refusal.
#
# WHY THIS FILE EXISTS.
#
# Both conditions below used to reach a compiler or a linker. One produced
#
#     fatal error: __config: No such file or directory
#
# from a `std` module source the package supplied and gcc could not consume;
# the other silently selected whichever supplier the graph traversal reached
# first and carried the loser's build inputs into the same command line.
#
# A message from a compiler about a target-side combination is a missing
# diagnostic: the engine knows the combination is untenable before it emits any
# command line. These assertions are that the engine says so.
set -e

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

# ── Rule two: an implementation is configured for the layer beneath it ──────
#
# The package declares a requirement the resolved compiler does not meet. It
# supplies no sources, so nothing here depends on the requirement being wrong
# in any particular way — only on it being stated.
mkdir -p runtime app/src
cat > runtime/mcpp.toml <<'TOML'
[package]
namespace = "probe"
name      = "needs-llvm"
version   = "0.1.0"
provides  = ["mcpp:c++-abi=libc++"]
requires  = ["mcpp:compiler=llvm"]

[build]
sources = []
TOML
cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
needs-llvm = { path = "../runtime" }
TOML
printf 'int main(){ return 0; }\n' > app/src/main.cpp

cd app
rc=0
out="$(MCPP_TOOLCHAIN=gcc@16.1.0 "$MCPP" build 2>&1)" || rc=$?
[[ "$rc" -ne 0 ]] || { echo "a requirement gcc does not meet was accepted:"; echo "$out"; exit 1; }
grep -q "requires the compiler to be" <<< "$out" || {
    echo "the refusal does not name the requirement:"; echo "$out"; exit 1; }
# THE NEXT STEP MUST BE ONE THAT WOULD ACTUALLY WORK, AND UNTIL 2026.8.26.2
# THE FIRST ONE OFFERED WAS `mcpp toolchain default llvm`.
#
# That is a GLOBAL change — the default for every project on the machine —
# printed because ONE project's dependency asked. And since the graph's
# requirement is now applied wherever mcpp's own answer was revisable, the only
# way to reach this refusal is a compiler the project STATED (here,
# `MCPP_TOOLCHAIN`). In that situation the global default is not what is being
# used, so changing it fixes nothing: the advice has to name the statement that
# decided.
grep -q "remove it" <<< "$out" || {
    echo "the refusal names no next step:"; echo "$out"; exit 1; }
# `if`, NOT `grep … && { … }`. Under `set -e` a trailing `&&` list whose
# left side fails takes the script down — and here grep FAILING is the passing
# case. This repo has paid for that shape more than once.
if grep -q "mcpp toolchain default" <<< "$out"; then
    echo "the refusal offers a global change that would not fix this failure:"
    echo "$out"; exit 1
fi
# The evidence the decision rests on must be printed even though the compiler
# layer comes from the payload and is suppressed in an ordinary report.
grep -qE "^\s+compiler\s+gcc" <<< "$out" || {
    echo "the refusal omits the layer it rests on:"; echo "$out"; exit 1; }
cd ..

# ── Rule one: one supplier per layer ────────────────────────────────────────
#
# A C library is a mutually exclusive choice, not an additive contribution.
# Selecting the wrong one does not fail the link; it produces a program that
# runs and intermittently does not.
mkdir -p a b app2/src
for n in a b; do
    cat > "$n/mcpp.toml" <<TOML
[package]
namespace = "probe"
name      = "libc-$n"
version   = "0.1.0"
provides  = ["mcpp:c-abi=tiny$n"]

[build]
sources = []
TOML
done
cat > app2/mcpp.toml <<'TOML'
[package]
name    = "app2"
version = "0.1.0"

[dependencies]
libc-a = { path = "../a" }
libc-b = { path = "../b" }
TOML
printf 'int main(){ return 0; }\n' > app2/src/main.cpp

cd app2
rc=0
out="$("$MCPP" build 2>&1)" || rc=$?
[[ "$rc" -ne 0 ]] || { echo "two suppliers of one layer were accepted:"; echo "$out"; exit 1; }
grep -q "two packages supply the c-abi" <<< "$out" || {
    echo "the refusal does not name the layer:"; echo "$out"; exit 1; }
for pkg in "libc-a@0.1.0" "libc-b@0.1.0"; do
    grep -q "$pkg" <<< "$out" || {
        echo "the refusal does not name $pkg:"; echo "$out"; exit 1; }
done

# ── The compiler is a layer no package may supply ───────────────────────────
cd ..
mkdir -p c app3/src
cat > c/mcpp.toml <<'TOML'
[package]
namespace = "probe"
name      = "claims-compiler"
version   = "0.1.0"
provides  = ["mcpp:compiler=llvm"]

[build]
sources = []
TOML
cat > app3/mcpp.toml <<'TOML'
[package]
name    = "app3"
version = "0.1.0"

[dependencies]
claims-compiler = { path = "../c" }
TOML
printf 'int main(){ return 0; }\n' > app3/src/main.cpp
cd app3
rc=0
out="$("$MCPP" build 2>&1)" || rc=$?
[[ "$rc" -ne 0 ]] || { echo "a package was allowed to supply the compiler:"; echo "$out"; exit 1; }
grep -q "not a layer a package can supply" <<< "$out" || {
    echo "the refusal does not say why:"; echo "$out"; exit 1; }

# ── The layer vocabulary must be extensible by a published package ──────────
#
# This one comes from a measurement, and the behaviour it replaces made the
# vocabulary permanently un-extendable:
#
#     error: dependency 'openkal-llvm-runtime': mcpp.toml: error:
#            `provides = ["mcpp:compiler-runtime=compiler-rt"]` names no
#            capability mcpp knows.
#
# A package declaring a newly named layer failed to load under every engine
# released before that layer was named. Whose manifest it is decides the
# answer: the root's is a misspelling the author is looking at, a dependency's
# is a manifest written against a newer engine.
cd ..
mkdir -p future app4/src
cat > future/mcpp.toml <<'TOML'
[package]
namespace = "probe"
name      = "from-the-future"
version   = "0.1.0"
provides  = ["mcpp:quantum-abi=spooky"]

[build]
sources = []
TOML
cat > app4/mcpp.toml <<'TOML'
[package]
name    = "app4"
version = "0.1.0"

[dependencies]
from-the-future = { path = "../future" }
TOML
printf 'int main(){ return 0; }\n' > app4/src/main.cpp
cd app4
rc=0
out="$("$MCPP" build 2>&1)" || rc=$?
[[ "$rc" -eq 0 ]] || {
    echo "a dependency naming a layer this engine does not know must not fail the build:"
    echo "$out"; exit 1; }
grep -q "quantum-abi" <<< "$out" || {
    echo "the ignored layer must be named:"; echo "$out"; exit 1; }

# And the same name in the ROOT's own manifest is the author's to fix.
cat > mcpp.toml <<'TOML'
[package]
name     = "app4"
version  = "0.1.0"
provides = ["mcpp:quantum-abi=spooky"]
TOML
rc=0
out="$("$MCPP" build 2>&1)" || rc=$?
[[ "$rc" -ne 0 ]] || {
    echo "a misspelled layer name in the root manifest must be an error:"
    echo "$out"; exit 1; }
grep -q "names no capability mcpp knows" <<< "$out" || {
    echo "the refusal does not say what is wrong:"; echo "$out"; exit 1; }

echo "OK"
