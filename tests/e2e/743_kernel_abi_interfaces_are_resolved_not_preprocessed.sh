#!/usr/bin/env bash
# requires: gcc
# 743 -- a package states which interfaces of the `kernel-abi` layer it uses,
# and a graph whose implementation does not provide one is refused BEFORE
# anything is compiled.
#
# WHY THIS IS THE RIGHT TIME TO ANSWER IT. openkal's specification (0.14
# §6.2) tabulates three times at which information about a capability becomes
# available and states that each is the EARLIEST at which it exists:
# dependency resolution answers "may this program be built against this
# implementation", the link answers "was an interface used that the
# implementation does not provide", a property word answers "how does it
# behave within an interface it provides". Source that asks the same question
# with `#ifdef` asks it during preprocessing -- earlier than any answer -- and
# that is why every macro-shaped answer to it has had to be replaced by the
# next one.
#
# Four legs:
#   A  a requirement the implementation provides: builds.
#   B  a requirement it does not: refused, naming the interface and both
#      packages, with nothing compiled.
#   C  a graph whose implementation states nothing: builds, AND SAYS SO. The
#      key postdates the provider, and a graph that has not adopted it must
#      keep building -- but a green build in which nobody checked reads
#      exactly like one in which everybody agreed, so the note names it.
#   D  the mirror of C: when the provider does state a list, that note is
#      ABSENT. Without this leg C would pass against an engine that printed
#      the note unconditionally, which measures nothing.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p impl/src src
cat > impl/src/lib.c <<'EOF'
int fake_kernel_marker(void) { return 0; }
EOF
cat > src/main.c <<'EOF'
int main(void) { return 0; }
EOF

write_impl() {  # $1 = the provides-interfaces array body, or empty for none
    if [ -z "$1" ]; then
        cat > impl/mcpp.toml <<'EOF'
[package]
name     = "fakekernel"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]

[targets.fakekernel]
kind    = "lib"
sources = ["src/*.c"]
EOF
    else
        cat > impl/mcpp.toml <<EOF
[package]
name     = "fakekernel"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]

[targets.fakekernel]
kind    = "lib"
sources = ["src/*.c"]

[kernel-abi]
provides-interfaces = [$1]
EOF
    fi
}

write_root() {  # $1 = the requires-interfaces array body, or empty for none
    if [ -z "$1" ]; then
        cat > mcpp.toml <<'EOF'
[package]
name    = "iface-probe"
version = "0.1.0"

[dependencies]
fakekernel = { path = "impl" }

[build]
allow_host_libs = true
EOF
    else
        cat > mcpp.toml <<EOF
[package]
name    = "iface-probe"
version = "0.1.0"

[dependencies]
fakekernel = { path = "impl" }

[build]
allow_host_libs = true

[kernel-abi]
requires-interfaces = [$1]
EOF
    fi
}

# ── A. every required interface is provided ───────────────────────────────
write_impl '"openkal.abort", "openkal.stream", "openkal.memory", "openkal.fs"'
write_root '"openkal.fs", "openkal.stream"'
out=$("$MCPP" build 2>&1) || {
    echo "FAIL: a graph whose implementation provides every required" \
         "interface must build" >&2
    echo "$out" >&2
    exit 1
}
echo "OK: A (every requirement provided)"

# ── B. one is not provided ────────────────────────────────────────────────
rm -rf target
write_root '"openkal.fs", "openkal.net"'
out=$("$MCPP" build 2>&1) && {
    echo "FAIL: a requirement the implementation does not provide must be" \
         "refused at resolution" >&2
    echo "$out" >&2
    exit 1
}
echo "$out" | grep -q "openkal.net" || {
    echo "FAIL: the refusal must name the interface that is missing" >&2
    echo "$out" >&2
    exit 1
}
echo "$out" | grep -q "iface-probe" || {
    echo "FAIL: the refusal must name the package that required it" >&2
    echo "$out" >&2
    exit 1
}
echo "$out" | grep -q "fakekernel" || {
    echo "FAIL: the refusal must name the implementation that did not" \
         "provide it -- a reader told only 'missing' cannot tell which of" \
         "the two to change" >&2
    echo "$out" >&2
    exit 1
}
# AND THE SENTENCE AROUND THOSE NAMES, NOT ONLY THE NAMES. Every assertion
# above matches an identifier, and an identifier sits in the right place under
# a wording that says the opposite. This message used to read
#
#     openkal.net
#     provided by  fakekernel (2 interfaces)
#
# --- the missing name, and directly beneath it a line that parses as
# "openkal.net is provided by fakekernel", which is what the refusal exists to
# deny. Nothing caught it because `grep -q fakekernel` is true either way.
echo "$out" | grep -q "the resolved implementation is fakekernel" || {
    echo "FAIL: the line naming the implementation must say that it is the" \
         "one that was RESOLVED. Placed under the missing interface with a" \
         "'provided by' label, it reads as the statement this refusal denies." >&2
    echo "     got: $(echo "$out" | grep -m1 'fakekernel')" >&2
    exit 1
}
echo "$out" | grep -qi "provided by *fakekernel" && {
    echo "FAIL: 'provided by fakekernel' directly under the missing interface" \
         "states the opposite of this refusal" >&2
    echo "$out" >&2
    exit 1
}
# AND THE CODE, BECAUSE SOMETHING READS THIS. mcpp-index's compatibility
# measurement tells "this graph does not supply what the member asked for"
# from "the member did not build" on this token; without it that consumer has
# to match prose, and the distinction decides whether a member counts against
# a compatibility figure.
echo "$out" | grep -q "\[interface-not-provided\]" || {
    echo "FAIL: the refusal must carry its code the way E0006 does" >&2
    echo "$out" >&2
    exit 1
}
# Nothing compiled: the answer exists before the compiler is reached, and a
# refusal that arrives after an object file has been written has answered at
# the wrong time even when it answers correctly.
if [ -d target ] && find target -name '*.o' -print -quit | grep -q .; then
    echo "FAIL: the refusal must arrive before anything is compiled" >&2
    find target -name '*.o' | head -3 >&2
    exit 1
fi
echo "OK: B (missing interface refused, nothing compiled)"

# ── C. the implementation states nothing ──────────────────────────────────
rm -rf target
write_impl ''
write_root '"openkal.fs", "openkal.net"'
out=$("$MCPP" build 2>&1) || {
    echo "FAIL: a provider that predates this key must keep building --" \
         "a package that states nothing is not a package that provides" \
         "nothing" >&2
    echo "$out" >&2
    exit 1
}
# AND THE SILENCE IS NAMED. Three situations exist and two build: the
# provider lists the interface (A), it lists others (B, refused), it lists
# nothing (here). Without this line the first and the third produce the same
# output, so a reader cannot tell "checked and agreed" from "never asked".
echo "$out" | grep -q "kernel-abi interfaces" || {
    echo "FAIL: a build in which no provider stated a list must say so --" \
         "otherwise an unanswered requirement reads as a confirmed one" >&2
    echo "$out" >&2
    exit 1
}
echo "$out" | grep -q "fakekernel" || {
    echo "FAIL: the note must name WHOSE silence this is; a reader told only" \
         "that something went unchecked cannot act on it" >&2
    echo "$out" | grep -m1 "kernel-abi interfaces" >&2
    exit 1
}
# THE COUNT, BECAUSE IT IS THE ONE PART THE FIXTURE DETERMINES. The root
# declares two requirements; a note that said "1" or "0" would still match
# every assertion above while reporting something other than what was asked.
echo "$out" | grep -q "2 requirements unchecked" || {
    echo "FAIL: the note must report how many requirements went unchecked;" \
         "this graph asked for two" >&2
    echo "$out" | grep -m1 "kernel-abi interfaces" >&2
    exit 1
}
echo "OK: C (a provider that states nothing is not refused, and is named)"

# ── D. the mirror: a provider that states a list produces no such note ────
rm -rf target
write_impl '"openkal.abort", "openkal.stream", "openkal.memory", "openkal.fs"'
write_root '"openkal.fs", "openkal.stream"'
out=$("$MCPP" build 2>&1) || {
    echo "FAIL: leg D's graph is leg A's and must build" >&2
    echo "$out" >&2
    exit 1
}
echo "$out" | grep -q "kernel-abi interfaces" && {
    echo "FAIL: the note belongs to the case where nothing was checked. A" \
         "graph whose provider DID state its list was checked, and printing" \
         "it here would make leg C pass against an engine that always" \
         "printed it" >&2
    echo "$out" | grep -m1 "kernel-abi interfaces" >&2
    exit 1
}
echo "OK: D (a provider that states its list draws no note)"

echo "OK"
