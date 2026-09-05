#!/usr/bin/env bash
# requires: import-std-libcxx
# The older toolchain spelling keeps working, and keeps meaning the same thing.
#
# WHY THIS FILE EXISTS.
#
# `openkal-llvm` was a toolchain family that named the same payload as `llvm`
# and existed to carry one fact: that a project's C library, C++ runtime and
# platform implementation come from packages rather than from a payload beside
# the compiler. That fact now belongs to `mcpp.targetside`, is read from what
# packages declare, and is resolved after the dependency graph exists — so the
# family name carries nothing and nothing branches on it.
#
# What must not happen is that a manifest written against the older spelling
# stops building. There is no deprecation deadline here on purpose: the spelling
# costs one row in a name table, and an engine that refuses a manifest it used to
# accept has broken a project that did nothing wrong.
#
# The assertion is about the RESOLVED DRIVER rather than about a successful
# build, because the two spellings are supposed to be indistinguishable at that
# point and a build additionally depends on what the project contains.
#
# `"$MCPP"`, never a bare `mcpp`: the harness passes the binary under test.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src
printf 'int main() { return 0; }\n' > src/main.cpp

manifest() {
    cat > mcpp.toml <<EOF
[package]
name    = "spelling-probe"
version = "0.1.0"

[toolchain]
default = "$1"
EOF
}

driver_for() {
    manifest "$1"
    rm -rf target
    "$MCPP" build 2>&1 | sed -n 's/.*Resolved [^ ]* → \(.*\)$/\1/p' | head -1
}

new_spelling=$(driver_for "llvm@22.1.8")
old_spelling=$(driver_for "openkal-llvm@22.1.8")

[ -n "$new_spelling" ] || {
    echo "could not read the resolved driver for the current spelling" >&2
    manifest "llvm@22.1.8"; "$MCPP" build 2>&1 | head -20 >&2
    exit 1
}

[ "$new_spelling" = "$old_spelling" ] || {
    echo "the two spellings must resolve to the same driver" >&2
    echo "  llvm@22.1.8         → $new_spelling" >&2
    echo "  openkal-llvm@22.1.8 → $old_spelling" >&2
    exit 1
}

# AND THE OLDER SPELLING MUST NOT STILL DECIDE ANYTHING. Both manifests here
# have an empty dependency graph, so both must report a target side supplied
# entirely by the payload. If the family name still carried the fact it used to,
# the second would report `graph` somewhere and the first would not.
manifest "openkal-llvm@22.1.8"
rm -rf target
# MCPP_VERBOSE, because an ordinary report prints only the layers the compiler
# payload did NOT supply — and every layer here is the payload's, which is
# precisely what this assertion is about.
old_report=$(MCPP_VERBOSE=1 "$MCPP" build 2>&1 | grep -E 'kernel-abi|c-abi' || true)
echo "$old_report" | grep -q 'graph' && {
    echo "the toolchain family name must no longer decide where the target side comes from" >&2
    echo "$old_report" >&2
    exit 1
}
echo "$old_report" | grep -q 'payload' || {
    echo "with an empty graph every layer comes from the payload" >&2
    echo "$old_report" >&2
    exit 1
}

echo "the openkal-llvm spelling resolves as llvm and decides nothing"
