#!/usr/bin/env bash
# requires: unix-shell
# `MCPP_TARGET_REQUESTED` — the value a build program needs and `MCPP_TARGET`
# cannot give it.
#
# WHY THE TWO VARIABLES ARE NOT THE SAME QUESTION.
#
# `MCPP_TARGET` answers "which machine is this for", and it is filled in with
# the host when nobody named a target — which is right for that question and
# makes it useless for a different one a platform package has to ask: **was this
# build POINTED at a target**, or is it an ordinary native build?
#
# The two differ even when the triples are equal. `mcpp build --target
# aarch64-macos` on an arm64 Mac names the very machine the host is, and yet it
# is the dependency graph that supplies the target side — so mcpp puts no system
# SDK on the link, and the package that knows the system is the only thing that
# can name one. A native build on the same machine gets the SDK and needs
# nothing from the package.
#
# Both readings of that question have already been measured wrong in
# `openkal-macos`:
#
#   from the host  → right for the cross, and `library not found for -lSystem`
#                    for `--target aarch64-macos` ON a Mac
#   from MCPP_TARGET → right for the cross, and `undefined symbol: wcslen` for
#                    the native build, because it is never empty and the
#                    package's three-name stub shadowed the vendor's complete one
#
# ⇒ The assertions below are the contract those two attempts needed: EMPTY for a
# native build, and the requested triple otherwise.
#
# `"$MCPP"`, never a bare `mcpp`: the harness passes the binary under test,
# and a bare name resolves through PATH to whichever engine is installed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "target-requested-probe"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > src/main.cpp

# The build program reports through a NON-ZERO exit, because mcpp prints what
# it captured only from a program that failed. A probe that returned zero would
# have its output discarded and this test would assert nothing.
cat > build.mcpp <<'EOF'
import mcpp;
import std;
int main() {
    const char* t = std::getenv("MCPP_TARGET");
    const char* r = std::getenv("MCPP_TARGET_REQUESTED");
    std::cerr << "PROBE target=[" << (t ? t : "") << "] "
              << "requested=[" << (r ? r : "") << "]\n";
    return 1;
}
EOF

# ── A native build: nobody named a target ──────────────────────────────────
native=$("$MCPP" build 2>&1 || true)
echo "$native" | grep -q 'PROBE ' || {
    echo "the build program did not report; mcpp prints a failing program's output" >&2
    echo "$native" >&2
    exit 1
}
echo "$native" | grep -qE 'requested=\[\]' || {
    echo "MCPP_TARGET_REQUESTED should be empty for a native build" >&2
    echo "$native" | grep 'PROBE ' >&2
    exit 1
}
# And the positive control on the same line: `MCPP_TARGET` must be filled in.
# Without this the assertion above would also pass if mcpp had stopped setting
# any of them.
echo "$native" | grep -qE 'target=\[[a-z0-9_]+-[a-z0-9-]+\]' || {
    echo "MCPP_TARGET should carry the host triple for a native build" >&2
    echo "$native" | grep 'PROBE ' >&2
    exit 1
}

# ── A build pointed at a target ────────────────────────────────────────────
#
# THE HOST'S OWN TRIPLE, NAMED EXPLICITLY — which is the case that
# distinguishes the two variables rather than merely one that differs from the
# native run. `--target <host>` produces equal values for `MCPP_TARGET` and the
# host triple, and `MCPP_TARGET_REQUESTED` is non-empty because a target was
# named. A test that used a foreign triple would pass with a variable that
# merely echoed `MCPP_TARGET`.
#
# And it needs no payload. The first version of this test named
# `x86_64-linux-musl` on the grounds that every host could resolve it, which was
# an assumption rather than a measurement:
#
#     error: target 'x86_64-linux-musl' cannot be built on this host —
#            no toolchain payload exists that runs here and produces it
#
# on the macOS leg. The host's own triple is the one target every host has by
# construction.
host_triple=$(echo "$native" | sed -n 's/.*PROBE target=\[\([^]]*\)\].*/\1/p' | head -1)
[ -n "$host_triple" ] || { echo "could not read the host triple from the probe" >&2; exit 1; }
cross=$("$MCPP" build --target "$host_triple" 2>&1 || true)
echo "$cross" | grep -q 'PROBE ' || {
    echo "the build program did not report on the cross build" >&2
    echo "$cross" >&2
    exit 1
}
echo "$cross" | grep -qE "requested=\[$host_triple\]" || {
    echo "MCPP_TARGET_REQUESTED should carry the triple that was named" >&2
    echo "$cross" | grep 'PROBE ' >&2
    exit 1
}

echo "MCPP_TARGET_REQUESTED distinguishes a native build from a named target"
