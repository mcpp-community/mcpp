#!/usr/bin/env bash
# requires: fresh-sandbox
# 686 -- a shell's `xlings subos use <name>` does not redirect mcpp's registry.
# That shell exports XLINGS_ACTIVE_SUBOS, which names a SubOS of the shell's
# xlings home, and xlings ranks it above a home's own `activeSubos`. mcpp's
# registry is a different home, and mcpp derives its tool and view paths from
# `subos/default`. Inherited, the variable made a fresh home's bootstrap install
# `ninja` and `patchelf` into `registry/subos/<name>/bin` (measured on
# 2026.9.14.2), where mcpp does not look, and left the pkg-config view that
# `mcpp::pkg_config_libdir()` names empty.
#
# Criteria, for a fresh home whose first command runs with the variable set:
#   A. `registry/subos/<name>` does not exist.
#   B. The bootstrap's tools are in `registry/subos/default/bin`. This is the
#      denominator: a bootstrap that installed nothing would meet A vacuously.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
cd "$TMP"
SHELL_SUBOS=mcpp-e2e-shell-subos
XLINGS_ACTIVE_SUBOS=$SHELL_SUBOS "$MCPP" self env > env.log 2>&1 || true

SUBOS="$MCPP_HOME/registry/subos"
[ -d "$SUBOS/default" ] || fail "the registry was not initialised" env.log

# ── A ──────────────────────────────────────────────────────────────────────
if [ -e "$SUBOS/$SHELL_SUBOS" ]; then
    fail "A: the registry acted on the shell's SubOS (bin: $(ls "$SUBOS/$SHELL_SUBOS/bin" 2>/dev/null | tr '\n' ' '))" env.log
fi

# ── B ──────────────────────────────────────────────────────────────────────
tools=$(ls "$SUBOS/default/bin" 2>/dev/null | grep -v -i '^xlings' | tr '\n' ' ')
[ -n "$tools" ] || fail "B: the bootstrap placed no tool in subos/default/bin" env.log
echo "ok: the bootstrap's tools are in subos/default/bin: $tools"

echo "PASS: 686_an_activated_xlings_subos_does_not_redirect_the_registry"
