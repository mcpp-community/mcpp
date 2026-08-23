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
# `"$MCPP"`, never a bare `mcpp`: the harness passes the binary under test, and
# a bare name resolves through PATH to whichever engine is installed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── A package that supplies two layers ──────────────────────────────────────
#
# One package standing in for the platform implementation and the C library
# both. The resolver reads a layer per capability, not a layer per package, so
# a single provider of two layers is a legitimate shape and a useful one to
# assert: it proves the two lookups are independent.
mkdir -p sys/src
cat > sys/mcpp.toml <<'EOF'
[package]
name     = "fake-system"
version  = "2.0.0"
provides = ["mcpp:kernel-abi=fakeos", "mcpp:c-abi=fakelibc"]

[targets.fake-system]
kind    = "lib"
sources = ["src/*.c"]
EOF
printf 'int fake_system_marker(void) { return 0; }\n' > sys/src/sys.c

mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "target-side-probe"
version = "0.1.0"

[dependencies]
fake-system = { path = "sys" }
EOF
printf 'int main() { return 0; }\n' > src/main.cpp

# ⚠️ `|| true`, AND THAT IS THE TEST'S SUBJECT RATHER THAN A CONCESSION.
# The resolution is reported during planning, before a single object is
# compiled, so what this file asserts is complete whether or not the link
# afterwards succeeds. Requiring a successful link would additionally require a
# working C runtime payload for the host, which is a different thing to test
# and one the rest of the suite already covers.
out=$("$MCPP" build 2>&1 || true)

# The report exists, and it names the layer, the interface and the provider.
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

# ⚠️ AND THE POSITIVE CONTROL ON THE SAME OUTPUT. Without this, the assertions
# above would also pass on an engine that printed the same three lines for
# every build regardless of what the graph contained.
echo "$out" | grep -q 'c++ *—' || {
    echo "a project with no C++ runtime package must report that layer absent" >&2
    echo "$out" >&2; exit 1
}

# ── The same project without the declarations ───────────────────────────────
#
# The control that makes the block above mean something: remove the capability
# line and every layer must fall back to the payload.
sed -i.bak '/^provides/d' sys/mcpp.toml
rm -rf target
plain=$("$MCPP" build 2>&1 || true)
echo "$plain" | grep -q 'fakeos' && {
    echo "a package that declares no capability must not fill a layer" >&2
    echo "$plain" >&2; exit 1
}
echo "$plain" | grep -qE 'kernel-abi .*payload' || {
    echo "with nothing in the graph, the layers come from the payload" >&2
    echo "$plain" >&2; exit 1
}
mv sys/mcpp.toml.bak sys/mcpp.toml

# ── A misspelling inside mcpp's reserved namespace is an error ──────────────
#
# This is the whole reason the prefix exists. An unvalidated capability array
# turns one wrong letter into a behaviour that silently does not happen, and
# the build still reports success.
sed -i 's/mcpp:kernel-abi=fakeos/mcpp:kernel_abi=fakeos/' sys/mcpp.toml
rm -rf target
bad=$("$MCPP" build 2>&1 || true)
echo "$bad" | grep -q "names no capability mcpp knows" || {
    echo "a misspelled capability in the mcpp: namespace must fail the build" >&2
    echo "$bad" >&2; exit 1
}
echo "$bad" | grep -q 'mcpp:kernel-abi' || {
    echo "the diagnostic must list the layer names that do exist" >&2
    echo "$bad" >&2; exit 1
}
# And it must fail BEFORE anything is compiled: a manifest this engine cannot
# read is not a build that got far enough to have a link.
echo "$bad" | grep -q 'Compiling' && {
    echo "the manifest must be rejected before compilation begins" >&2
    echo "$bad" >&2; exit 1
}

# ── A name outside the namespace is none of mcpp's business ────────────────
#
# `provides` also carries capabilities packages match among themselves. Closing
# the whole array would reject those, and the freestanding allocator selection
# that already ships is one.
sed -i 's/"mcpp:kernel_abi=fakeos", //' sys/mcpp.toml
sed -i 's/provides = \[/provides = ["a-capability-mcpp-never-heard-of", /' sys/mcpp.toml
rm -rf target
free=$("$MCPP" build 2>&1 || true)
echo "$free" | grep -q "names no capability mcpp knows" && {
    echo "a capability outside the mcpp: namespace must pass through untouched" >&2
    echo "$free" >&2; cat sys/mcpp.toml >&2; exit 1
}

echo "target-side resolution reads the graph, reports it, and validates its own namespace"
