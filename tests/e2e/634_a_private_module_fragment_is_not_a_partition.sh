#!/usr/bin/env bash
# requires: gcc
# A PRIVATE MODULE FRAGMENT IS NOT AN IMPLEMENTATION PARTITION.
#
# `module : private;` ([module.private.frag]) is a third production, not a
# spelling of the two the scanner already handled:
#
#   module M;          implementation unit      -> requires M
#   module M:part;     implementation partition -> provides M:part
#   module : private;  private module fragment  -> neither
#
# It was read as the second, because the scanner's name tokeniser admits `:`
# so that `M:part` scans as one token, and the partition test is "the name
# contains a colon". The colon in this production comes FIRST and belongs to no
# name, so a valid file was refused at scan time with `file already provides
# module 'M'; cannot also provide ':'` -- a sentence that is false about the
# source it names. A regression from #433, first released in v2026.8.18.1.
#
# THE CRITERION IS THE GRAPH, NOT THE BUILD'S EXIT STATUS, and that is forced by
# the compiler rather than chosen. GCC 16.1 answers `sorry, unimplemented:
# private module fragment`, so on the toolchain this shard has, a correct mcpp
# still cannot produce a binary. What a correct mcpp does is get out of the way:
# it scans the file, emits the graph, and lets the compiler answer for its own
# feature set. Before the fix mcpp exited before writing any graph at all.
#
# Both spellings are exercised because the tokeniser treats them differently --
# `module : private;` yields the "name" `:` and `module :private;` yields
# `:private` -- so one passing says nothing about the other.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

for spelling in "module : private;" "module :private;"; do
    rm -rf proj
    mkdir -p proj/src
    cat > proj/src/pf.cppm <<EOF
export module pf;
export int pf();

$spelling

int pf() { return 42; }
EOF
    cat > proj/src/main.cpp <<'EOF'
import pf;
int main() { return pf() == 42 ? 0 : 1; }
EOF
    cat > proj/mcpp.toml <<'EOF'
[package]
name    = "pf"
version = "0.1.0"
EOF

    cd proj
    set +e
    "${MCPP:-mcpp}" build > build.log 2>&1
    build_status=$?
    set -e

    # The discriminating leg. This string is what the defect emitted, and it is
    # a claim about the user's source.
    if grep -q "cannot also provide" build.log; then
        echo "FAIL [$spelling]: the private module fragment was read as a partition"
        cat build.log
        exit 1
    fi
    # The identity guard must not fire on it either.
    if grep -q "is not a module name" build.log; then
        echo "FAIL [$spelling]: the fragment was rejected as a malformed identity"
        cat build.log
        exit 1
    fi

    # THE POSITIVE LEG. An absence alone would also be satisfied by an mcpp that
    # failed earlier for an unrelated reason, so assert the artifact a correct
    # scan produces: a graph, with a module edge for this file.
    graph=$(find target -name build.ninja | head -1)
    [ -n "$graph" ] || { echo "FAIL [$spelling]: no build graph was written"
                         cat build.log; exit 1; }
    grep -q "cxx_module .*pf\.cppm" "$graph" || {
        echo "FAIL [$spelling]: no module edge for pf.cppm"
        grep 'pf\.cppm' "$graph" || true
        exit 1; }

    # Where the toolchain implements the feature (clang today, GCC when it
    # lands) the program must also RUN. Where it does not, the compiler's own
    # sentence is the right answer and mcpp adds nothing to it.
    if [ $build_status -eq 0 ]; then
        bin=$(find target -type f -name pf -perm -u+x | head -1)
        [ -n "$bin" ] || { echo "FAIL [$spelling]: build succeeded with no binary"; exit 1; }
        "$bin" || { echo "FAIL [$spelling]: the program did not return 42"; exit 1; }
        echo "ok [$spelling]: built and ran"
    else
        # The failure must be the COMPILER's, at the fragment's own line.
        # GCC 16.1 declines the feature twice and with two different sentences:
        # `module already declared` from the p1689 scan, and `sorry,
        # unimplemented: private module fragment` from codegen. Matching either
        # sentence would bind this test to a wording; matching the LINE binds it
        # to the claim that actually matters -- mcpp got out of the way, and
        # whatever declined did so at the declaration itself.
        grep -q "pf\.cppm:4" build.log || {
            echo "FAIL [$spelling]: build failed, but not at the fragment"
            cat build.log
            exit 1; }
        grep -q "scanner errors" build.log && {
            echo "FAIL [$spelling]: mcpp's own scan refused the file"
            cat build.log
            exit 1; }
        echo "ok [$spelling]: scanned; the compiler declined the feature"
    fi
    cd ..
done
