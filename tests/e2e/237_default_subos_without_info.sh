#!/usr/bin/env bash
# requires: elf
# A DEFAULT SubOS that predates `subos_info` must not stop the build (#427).
#
# THE REGRESSION THIS PINS
#
# `ensure_post_install_fixup` needs a C runtime to bind a freshly installed
# toolchain payload to. It took that identity from the caller's RuntimeBinding
# and, when the caller had none, read `<xlings home>/subos/default` itself and
# turned a missing description into a hard error:
#
#   error: toolchain post-install fixup: cannot fix up gcc toolchain '…':
#          default SubOS has no RuntimeBinding identity (… no `subos_info` block)
#
# Shipped in 2026.8.10.x; 2026.8.8.4 was fine. Measured trigger: a SubOS whose
# `.xlings.json` is `{"workspace":{}}` — 16 bytes, written by an xlings older
# than the block. mcpp's own message says as much ("A newer xlings writes this
# block"), i.e. it KNEW this was a version difference and failed anyway.
#
# It is the index-floor rule and mcpp#221 again: DATA THAT IS MISSING OR NEWER
# MUST NOT INVALIDATE THE PROGRAM THAT READS IT.
#
# ⚠️ WHY `221_subos_without_info_still_builds.sh` DOES NOT COVER THIS.
#
# 221 creates a PROJECT-LOCAL SubOS (`[xlings] subos = "bare"` →
# `<project>/.mcpp/.xlings/subos/bare`). The fixup read a HARDCODED
# `<xlings home>/subos/default`. The two never intersect, so 221's empty SubOS
# reached none of this code, and on any machine whose real `default` describes
# itself the gate simply passed. A test for "absence degrades" has to put the
# absence on THE OBJECT THAT IS READ.
#
# WHAT THIS FILE CAN AND CANNOT REACH
#
# The gate returns early for a payload that resolves outside the caller's
# registry ("inherited payload, owner is responsible for its fixup"), and this
# test inherits its toolchain by symlink so it costs no download. So the gate's
# own branches — degrade vs. contradiction, marker vs. no marker — are pinned in
# tests/unit/test_post_install.cpp, where the payload is a real directory inside
# the test's registry. What is asserted HERE is the user-visible contract: the
# build no longer dies on the fixup, and `allow_host_libs` reaches the link.
set -e

_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -z "${MCPP:-}" ]; then
  MCPP="$(bash "$_root/.github/tools/newest_artifact.sh" "$_root" mcpp 2>/dev/null || true)"
  [ -n "$MCPP" ] || { echo "SKIP: no mcpp binary built yet"; exit 0; }
fi
case "$MCPP" in /*) ;; *) MCPP="$_root/$MCPP" ;; esac
[ -x "$MCPP" ] || { echo "FAIL: MCPP=$MCPP is not executable"; exit 1; }

TMP=$(mktemp -d)
trap "chmod -R u+w $TMP 2>/dev/null; rm -rf $TMP || true" EXIT

# A HOME of our own. The toolchain payload is inherited from the real one by
# symlink so this test costs no download — the fixup's containment guard skips
# patching an inherited payload anyway, which is exactly the "no patching
# happens" state this test is about.
export MCPP_HOME="$TMP/home"
REAL_HOME="${MCPP_HOME_REAL:-$HOME/.mcpp}"
mkdir -p "$MCPP_HOME/registry/data" "$MCPP_HOME/registry/subos/default"
if [ -d "$REAL_HOME/registry/data/xpkgs" ]; then
    ln -s "$REAL_HOME/registry/data/xpkgs" "$MCPP_HOME/registry/data/xpkgs"
else
    echo "SKIP: no xpkgs payloads to inherit from $REAL_HOME"; exit 0
fi
for d in "$REAL_HOME/registry"/*; do
    b="$(basename "$d")"
    case "$b" in data|subos) continue ;; esac
    ln -s "$d" "$MCPP_HOME/registry/$b" 2>/dev/null || true
done

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > mcpp.toml <<'EOF'
[package]
name    = "nosubosinfo"
version = "0.1.0"

[toolchain]
default = "gcc@16.1.0"
macos   = "llvm@22.1.8"
windows = "llvm@20.1.7"
EOF
echo 'int main() { return 0; }' > src/main.cpp

# ── 1. absence: the exact 16 bytes an old xlings leaves behind ──────────────
printf '{"workspace":{}}' > "$MCPP_HOME/registry/subos/default/.xlings.json"

set +e
out="$("$MCPP" build 2>&1)"
rc=$?
set -e
echo "$out" | sed 's/^/    /'

case "$out" in
    *"has no RuntimeBinding identity"*)
        echo "FAIL: an undescribed DEFAULT SubOS still kills the build."
        echo "      Absence leaves some facts unknown; it does not make the"
        echo "      build wrong. The fixup degrades — it must not decide."
        exit 1 ;;
esac
# The build MAY still stop here, but only for a reason that is about the C
# RUNTIME rather than about the toolchain fixup — and that distinction is the
# whole fix. With no declared runtime there is nothing to bind to, mcpp
# declines to guess a glibc version, and the hermeticity guard says so in terms
# the user can act on. Same accepted outcome as 221, and its wording is checked
# here so a future failure cannot inherit this test's blessing.
if [ "$rc" != 0 ]; then
    case "$out" in
        *"hermetic link check failed"*) ;;
        *)
            echo "FAIL: mcpp build exited $rc for a reason this test does not"
            echo "      recognise. An undescribed SubOS may cost hermeticity;"
            echo "      it must not cost anything else."
            exit 1 ;;
    esac
fi
echo "  absence: no fixup error (exit $rc)"

# ── 2. ...and with the host runtime allowed, it must BUILD ─────────────────
# The green half. Without it, part 1 could be satisfied by mcpp failing for
# some accepted reason on every platform, which is not what "the build is
# unaffected" means.
#
# ⚠️ THIS IS WHAT WAS BROKEN. In the reported sandbox, `allow_host_libs = true`
# did NOT help: the fixup gate fired before the hermeticity policy was ever
# consulted, so the one escape hatch mcpp documents for this exact situation
# was unreachable. Absence must cost hermeticity and nothing else.
cat >> mcpp.toml <<'EOF'

[build]
allow_host_libs = true
EOF
rm -rf target
"$MCPP" build > allow.log 2>&1 || {
    echo "FAIL: even with allow_host_libs, an undescribed default SubOS blocks"
    echo "      the build. That is the escape hatch being unreachable."
    tail -20 allow.log | sed 's/^/      /'
    exit 1; }
echo "  absence + allow_host_libs: builds"

# ── 3. `mcpp toolchain install` survives it too ─────────────────────────────
# The path the fix could most easily have BROKEN: it carries no RuntimeBinding
# of its own, so removing the fallback without also giving it a resolver would
# have made it skip the fixup forever — a silently incomplete install in place
# of a loud failure. See tests/unit/test_post_install.cpp for the gate itself.
set +e
iout="$("$MCPP" toolchain install gcc@16.1.0 2>&1)"
irc=$?
set -e
[ "$irc" = 0 ] || {
    echo "FAIL: \`mcpp toolchain install\` exited $irc with an undescribed SubOS"
    echo "$iout" | sed 's/^/      /'
    exit 1; }
case "$iout" in
    *"has no RuntimeBinding identity"*)
        echo "FAIL: the install path still turns an absent description into an error"
        exit 1 ;;
esac
echo "  toolchain install: exit 0"

echo "237 an undescribed default SubOS costs hermeticity and nothing else OK"
