#!/usr/bin/env bash
# requires: gcc
# 737 -- the C runtime a SubOS declares is installed before the toolchain fixup
# consumes it (mcpp#660).
#
# THE DEFECT. The default SubOS declares its glibc by exact version, and the
# gcc fixup patches against the payload directory of that version. Nothing
# installed that payload on purpose: the two loops meant to do it passed
# `xim:glibc` without a version, which the fetcher rejects, and the rejection
# was discarded. The payload arrived only as a dependency of a toolchain that
# xlings installed. A toolchain restored from a CI cache is not installed again,
# so a home whose cache held a different glibc revision failed with
#
#   error: toolchain post-install fixup: selected RuntimeBinding glibc@<v>
#          requires payload '.../xim-x-glibc/<v>', ...
#
# THE SHAPE. A CI cache keeps `registry/data/xpkgs` and nothing else: no xlings
# version database, no SubOS. This test installs gcc once, reduces the home to
# that shape, and renames the declared glibc payload so that the store holds a
# different directory for glibc, as a cache from before an index revision does.
# The neighbour's name is not a dotted refinement of the declared version, so
# the directory scan this change removes could not have accepted it either;
# the test is red on the previous implementation.
#
# Criteria:
#   A. Offline, the build fails and the message names the exact coordinate
#      `xim:glibc@<declared>` that provides the payload.
#   B. Online, the build succeeds, the declared payload directory now exists
#      with a loader, and the program runs.
#   C. `mcpp toolchain install` in the same shape succeeds as well (the second
#      entry point that runs the fixup).
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
export MCPP_NO_AUTO_INSTALL=1

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

set_mirror() {
    if [[ -n "${MCPP_E2E_TOOLCHAIN_MIRROR:-}" ]]; then
        "$MCPP" self config --mirror "$MCPP_E2E_TOOLCHAIN_MIRROR" > /dev/null
    fi
}

declared_runtime() {
    sed -n 's/.*"runtime": *"\(glibc@[^"]*\)".*/\1/p' \
        "$MCPP_HOME/registry/subos/default/.xlings.json" | head -1
}

# Reduce the home to what a CI cache restores, and move the declared glibc
# payload aside under a name that is not a version of it.
cache_shape() {
    local keep="$TMP/xpkgs-kept"
    mv "$MCPP_HOME/registry/data/xpkgs" "$keep"
    rm -rf "$MCPP_HOME"
    mkdir -p "$MCPP_HOME/registry/data"
    mv "$keep" "$MCPP_HOME/registry/data/xpkgs"
    local glibc="$MCPP_HOME/registry/data/xpkgs/xim-x-glibc"
    [[ -d "$glibc/$1" ]] || fail "cache shape: no payload for $1 under $glibc"
    mv "$glibc/$1" "$glibc/$1-cached-revision"
}

mkdir -p "$TMP/proj/src"
cat > "$TMP/proj/mcpp.toml" <<'EOF'
[package]
name    = "runtimeprobe"
version = "0.1.0"
EOF
cat > "$TMP/proj/src/main.cpp" <<'EOF'
import std;
int main() { std::println("runtime ok"); return 0; }
EOF

# ── Install once, in a fresh home ────────────────────────────────────────────
set_mirror
"$MCPP" toolchain install gcc 16.1.0 > "$TMP/install1.log" 2>&1 \
    || fail "initial toolchain install" "$TMP/install1.log"
DECLARED=$(declared_runtime)
[[ -n "$DECLARED" ]] || fail "the default SubOS declares no glibc runtime" \
    "$MCPP_HOME/registry/subos/default/.xlings.json"
VERSION=${DECLARED#glibc@}
echo "declared runtime: $DECLARED"

# ── A: offline, in the cache shape ───────────────────────────────────────────
cache_shape "$VERSION"
set_mirror
"$MCPP" toolchain default gcc@16.1.0 > "$TMP/default.log" 2>&1 \
    || fail "toolchain default in the cache shape" "$TMP/default.log"
[[ "$(declared_runtime)" == "$DECLARED" ]] \
    || fail "the re-initialised SubOS declares $(declared_runtime), not $DECLARED"
[[ ! -e "$MCPP_HOME/registry/data/xpkgs/xim-x-glibc/$VERSION" ]] \
    || fail "A: the declared payload exists before the build; the shape is wrong"

cd "$TMP/proj"
rc=0
MCPP_OFFLINE=1 "$MCPP" build > "$TMP/offline.log" 2>&1 || rc=$?
[[ $rc -ne 0 ]] || fail "A: an offline build without the declared payload succeeded" "$TMP/offline.log"
grep -q "xim:$DECLARED" "$TMP/offline.log" \
    || fail "A: the offline failure does not name xim:$DECLARED" "$TMP/offline.log"
echo "ok: A, offline build names xim:$DECLARED"

# ── B: online, the build installs the declared payload ───────────────────────
rm -rf target
"$MCPP" build > "$TMP/online.log" 2>&1 || fail "B: online build" "$TMP/online.log"
payload="$MCPP_HOME/registry/data/xpkgs/xim-x-glibc/$VERSION"
ls "$payload"/lib64/ld-linux-* "$payload"/lib/ld-linux-* > /dev/null 2>&1 \
    || fail "B: no loader under $payload after the build" "$TMP/online.log"
out=$("$MCPP" run 2>&1) || fail "B: run" "$TMP/online.log"
[[ "$out" == *"runtime ok"* ]] || fail "B: unexpected run output: $out"
echo "ok: B, online build installed $DECLARED and the program runs"

# ── C: toolchain install, in the cache shape ────────────────────────────────
cd "$TMP"
cache_shape "$VERSION"
set_mirror
"$MCPP" toolchain install gcc 16.1.0 > "$TMP/install2.log" 2>&1 \
    || fail "C: toolchain install in the cache shape" "$TMP/install2.log"
ls "$payload"/lib64/ld-linux-* "$payload"/lib/ld-linux-* > /dev/null 2>&1 \
    || fail "C: no loader under $payload after toolchain install" "$TMP/install2.log"
echo "ok: C, toolchain install installed $DECLARED"

echo "OK"
