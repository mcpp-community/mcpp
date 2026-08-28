#!/usr/bin/env bash
# requires: elf
# 306_dependency_linkage_axis.sh — `[build] dependency_linkage` (issue #519).
#
# The consumer, not the package author, decides whether a dependency arrives
# merged into the image or as a separate shared library. Three things have to
# hold and each of them has bitten before:
#
#   1. the DEFAULT changes nothing — an existing project must keep its output
#      directory and its build.ninja, or every user pays for a key they did
#      not write;
#   2. asking for `shared` actually produces a shared object the program LOADS
#      (not merely one that gets built and then ignored beside a statically
#      merged copy);
#   3. the two configurations live in DIFFERENT output directories. They do
#      not merely differ in flags: `-fPIC` is whole-build, so sharing a
#      directory means sharing objects between two incompatible compilations.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p core/src app/src

cat > core/mcpp.toml <<'EOF'
[package]
name    = "core"
version = "0.1.0"
[build]
c_standard = "c11"
sources    = ["src/*.c"]
[targets.core]
kind = "lib"
EOF
cat > core/src/core.c <<'EOF'
int core_answer(void) { return 42; }
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[build]
c_standard = "c11"
[targets.app]
kind = "bin"
main = "src/main.c"
[dependencies]
core = { path = "../core" }
EOF
cat > app/src/main.c <<'EOF'
#include <stdio.h>
int core_answer(void);
int main(void) { printf("%d\n", core_answer()); return 0; }
EOF

cd app

# ── 1. the default is the old behaviour ────────────────────────────────────
"$MCPP" build > static.log 2>&1 || { cat static.log; exit 1; }
# ⚠️ The FINGERPRINT directory, `target/<triple>/<fp>/`, not the triple one.
# `ls target/*/ -d` yields the triple, and comparing a fingerprint against it
# in step 3 would pass no matter what — a criterion aimed at the wrong object
# does not report an error, it quietly answers a different question.
static_fp="$(basename "$(ls -d target/*/*/ | head -1)")"
[[ "$static_fp" != "$(basename "$(ls -d target/*/ | head -1)")" ]] || {
    echo "FAIL: could not locate the fingerprint directory"; ls -R target | head; exit 1; }
count_before="$(ls -d target/*/*/ | wc -l)"
[[ "$count_before" == 1 ]] || { echo "FAIL: expected exactly one build dir, got $count_before"; exit 1; }

bin="$(find target -name app -type f | head -1)"
[[ -n "$bin" ]] || { cat static.log; echo "no executable"; exit 1; }
"$bin" | grep -qx 42 || { echo "static build does not run"; exit 1; }

# No shared object at all: a `kind = "lib"` dependency stays merged.
if find target -name 'libcore.so' | grep -q .; then
    echo "FAIL: the default produced a shared library"; exit 1
fi

# ── 2. asking for shared produces one, and the program loads it ────────────
sed -i 's/^c_standard = "c11"$/c_standard = "c11"\ndependency_linkage = "shared"/' mcpp.toml
grep -q 'dependency_linkage = "shared"' mcpp.toml || { echo "sed did not apply"; exit 1; }

"$MCPP" build > shared.log 2>&1 || { cat shared.log; exit 1; }
so="$(find target -name 'libcore.so' -type f | head -1)"
[[ -n "$so" ]] || { cat shared.log; echo "FAIL: no shared library was built"; exit 1; }
file "$so" | grep -q 'ELF.*shared object' || { echo "FAIL: not an ELF .so"; exit 1; }

sbin="$(find target -name app -type f -newer "$so" | head -1)"
[[ -n "$sbin" ]] || sbin="$(dirname "$so")/app"
[[ -x "$sbin" ]] || { echo "FAIL: no executable beside the .so"; exit 1; }

# The WHOLE point: the symbol must be imported, not merged. Asserted on the
# artifact rather than on a log line — a message is free to be reworded, and a
# "shared library exists" check passes just as well when the executable also
# contains a static copy of every symbol in it.
readelf --dyn-syms -W "$sbin" | grep -qE 'UND +core_answer' || {
    echo "FAIL: core_answer is not an imported symbol in the executable"
    readelf --dyn-syms -W "$sbin" | grep core_answer || true
    exit 1
}
readelf -d -W "$sbin" | grep -q 'NEEDED.*libcore.so' || {
    echo "FAIL: the executable does not declare libcore.so as NEEDED"; exit 1; }

"$sbin" | grep -qx 42 || { echo "FAIL: the shared build does not run"; exit 1; }

# ── 3. the two configurations do not share an output directory ─────────────
count_after="$(ls -d target/*/*/ | wc -l)"
[[ "$count_after" == 2 ]] || {
    echo "FAIL: expected two build directories (static + shared), got $count_after"
    ls -d target/*/*/
    exit 1
}
shared_fp=""
for d in target/*/*/; do
    b="$(basename "$d")"
    [[ "$b" != "$static_fp" ]] && shared_fp="$b"
done
[[ -n "$shared_fp" ]] || {
    echo "FAIL: both configurations landed in one output directory ($static_fp)"
    exit 1
}

echo "ok: default static ($static_fp), shared ($shared_fp), symbol imported"
