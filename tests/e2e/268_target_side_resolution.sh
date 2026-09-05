#!/usr/bin/env bash
# requires: unix-shell
# Target-side resolution: which layer of a target comes from where.
#
# WHAT THIS COVERS THAT THE UNIT TESTS CANNOT.
#
# `mcpp.targetside` is a pure function and its table is asserted directly in
# tests/unit/test_targetside.cpp. What only a build can show is the wiring: that
# a package's `provides` line actually reaches the resolver, that the resolution
# reaches the report, and that a misspelling in mcpp's reserved namespace stops
# the build instead of quietly disabling the behaviour it was meant to select.
#
# The packages here are local and trivial on purpose. The real ecosystem
# exercise lives in .github/workflows/openkal-cross.yml, which builds nine
# artifacts on three hosts; what this file needs is the mechanism, not the
# stack, and a test that fetched an ecosystem over the network to assert a
# string in a report would be slower and no more conclusive.
#
# NO `sed -i` ANYWHERE, AND THAT IS NOT STYLE. BSD sed reads the argument
# after `-i` as a backup suffix, so an in-place edit written for GNU sed fails
# on macOS. Measured on the macOS leg of this suite, in this very file:
#
#     sed: 1: "sys/mcpp.toml": unterminated substitute pattern
#
# The provider's manifest is therefore written from scratch for each case.
#
# `"$MCPP"`, never a bare `mcpp`: the harness passes the binary under test, and
# a bare name resolves through PATH to whichever engine is installed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p sys/src
printf 'int fake_system_marker(void) { return 0; }\n' > sys/src/sys.c

# Rewrite the provider's manifest with the capabilities given as arguments, and
# drop the output tree so the next build re-plans from it.
provider_declares() {
    {
        echo '[package]'
        echo 'name     = "fake-system"'
        echo 'version  = "2.0.0"'
        if [ $# -gt 0 ]; then
            printf 'provides = ['
            sep=""
            for cap in "$@"; do printf '%s"%s"' "$sep" "$cap"; sep=", "; done
            printf ']\n'
        fi
        echo ''
        echo '[targets.fake-system]'
        echo 'kind    = "lib"'
        echo 'sources = ["src/*.c"]'
    } > sys/mcpp.toml
    rm -rf target
}

mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "target-side-probe"
version = "0.1.0"

[dependencies]
fake-system = { path = "sys" }
EOF
printf 'int main() { return 0; }\n' > src/main.cpp

# ── A package that supplies two layers ──────────────────────────────────────
#
# One package standing in for the platform implementation and the C library
# both. The resolver reads a layer per capability, not a layer per package, so
# a single provider of two layers is a legitimate shape and a useful one to
# assert: it proves the two lookups are independent.
#
# `|| true`, AND THAT IS THE TEST'S SUBJECT RATHER THAN A CONCESSION. The
# resolution is reported during planning, before a single object is compiled, so
# what this file asserts is complete whether or not the link afterwards
# succeeds. Requiring a successful link would additionally require a working C
# runtime payload for the host, which is a different thing to test and one the
# rest of the suite already covers.
provider_declares "mcpp:kernel-abi=fakeos" "mcpp:c-abi=fakelibc"
out=$("$MCPP" build 2>&1 || true)

echo "$out" | grep -q 'kernel-abi  *fakeos' || {
    echo "the kernel-abi layer must report the interface the package declared" >&2
    echo "$out" >&2; exit 1
}
echo "$out" | grep -q 'c-abi  *fakelibc' || {
    echo "the c-abi layer must report the interface the package declared" >&2
    echo "$out" >&2; exit 1
}
echo "$out" | grep -qE 'fake-system@2\.0\.0' || {
    echo "the report must name the providing package and its version" >&2
    echo "$out" >&2; exit 1
}
echo "$out" | grep -q 'graph' || {
    echo "a layer supplied by a dependency must be reported as coming from the graph" >&2
    echo "$out" >&2; exit 1
}

# AND THE POSITIVE CONTROL ON THE SAME OUTPUT. Without this, the assertions
# above would also pass on an engine that printed the same three lines for
# every build regardless of what the graph contained.
# The label is the capability name — `c++-abi`, matching `mcpp:c++-abi` — so
# that the report and the manifest use one vocabulary for one layer.
echo "$out" | grep -q 'c++-abi *—' || {
    echo "a project with no C++ runtime package must report that layer absent" >&2
    echo "$out" >&2; exit 1
}

# ── The same project without the declarations ───────────────────────────────
#
# The control that makes the block above mean something: remove the capability
# line and every layer must fall back to the payload.
provider_declares
# MCPP_VERBOSE, because an ordinary report prints only the layers the compiler
# payload did NOT supply. That suppression is itself the control's subject here:
# with nothing in the graph there is nothing to print, and the assertion below
# needs the full stack to check that the fallback happened rather than that the
# lines merely vanished.
plain=$(MCPP_VERBOSE=1 "$MCPP" build 2>&1 || true)
echo "$plain" | grep -q 'fakeos' && {
    echo "a package that declares no capability must not fill a layer" >&2
    echo "$plain" >&2; exit 1
}
echo "$plain" | grep -qE 'kernel-abi .*payload' || {
    echo "with nothing in the graph, the layers come from the payload" >&2
    echo "$plain" >&2; exit 1
}

# ── A name inside mcpp's reserved namespace is never silent ─────────────────
#
# This is the whole reason the prefix exists: an unvalidated capability array
# turns one wrong letter into a behaviour that silently does not happen while
# the build reports success.
#
# IN A DEPENDENCY IT IS A WARNING RATHER THAN AN ERROR, AND THE CHANGE WAS
# FORCED BY A MEASUREMENT. mcpp cannot distinguish a misspelling from a layer
# named after this build tool was released, and the two mistakes cost different
# amounts. Refusing meant a package declaring a NEW layer failed to load under
# every engine released before it — the vocabulary could never be extended by a
# published package, permanently. Warning means a dependency's typo costs a
# missing layer the consumer is told about, and cannot fix anyway.
#
# What this file asserts is therefore the property the prefix was introduced
# for — the name is reported — not the severity. The root project's own
# manifest still errors; e2e 281 covers that half.
provider_declares "mcpp:kernel_abi=fakeos" "mcpp:c-abi=fakelibc"
bad=$("$MCPP" build 2>&1 || true)
echo "$bad" | grep -q "kernel_abi" || {
    echo "a misspelled capability in the mcpp: namespace must be reported" >&2
    echo "$bad" >&2; exit 1
}
echo "$bad" | grep -q 'mcpp:kernel-abi' || {
    echo "the report must list the layer names that do exist" >&2
    echo "$bad" >&2; exit 1
}
# And the misspelled layer must be UNFILLED — reporting the name and then
# resolving the layer anyway would be the worst of both.
echo "$bad" | grep -qE 'kernel-abi +fakeos' && {
    echo "a name this engine does not know must not fill a layer" >&2
    echo "$bad" >&2; exit 1
}
# The layer beside it, spelled correctly, still resolves: one bad entry costs
# one layer and not the package.
echo "$bad" | grep -qE 'c-abi +fakelibc' || {
    echo "a correctly spelled capability beside a misspelled one must still resolve" >&2
    echo "$bad" >&2; exit 1
}

# ── A name outside the namespace is none of mcpp's business ────────────────
#
# `provides` also carries capabilities packages match among themselves. Closing
# the whole array would reject those, and the freestanding allocator selection
# that already ships is one.
provider_declares "a-capability-mcpp-never-heard-of" "mcpp:c-abi=fakelibc"
free=$("$MCPP" build 2>&1 || true)
echo "$free" | grep -q "names no capability mcpp knows" && {
    echo "a capability outside the mcpp: namespace must pass through untouched" >&2
    echo "$free" >&2; cat sys/mcpp.toml >&2; exit 1
}
echo "$free" | grep -q 'c-abi  *fakelibc' || {
    echo "the layer beside the unknown name must still resolve" >&2
    echo "$free" >&2; exit 1
}

echo "target-side resolution reads the graph, reports it, and validates its own namespace"
