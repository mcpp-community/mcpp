#!/usr/bin/env bash
# _llvm_env.sh — resolve the LLVM toolchain the llvm e2e tests run against.
#
# Tests used to pin llvm@20.1.7, which meant zero coverage of newer payloads
# (a 22.x-only regression sailed through). Sourcing this sets:
#   LLVM_VERSION  — $MCPP_E2E_LLVM_VERSION if set, else the newest installed
#   LLVM_ROOT     — the payload root for that version
# Callers still SKIP when LLVM_ROOT doesn't exist (nothing installed).
#
# Usage:   source "$(dirname "$0")/_llvm_env.sh"

_llvm_base="${HOME}/.mcpp/registry/data/xpkgs/xim-x-llvm"
if [[ ! -d "$_llvm_base" && -n "${USERPROFILE:-}" ]]; then
    _llvm_base="${USERPROFILE}/.mcpp/registry/data/xpkgs/xim-x-llvm"
fi

if [[ -n "${MCPP_E2E_LLVM_VERSION:-}" ]]; then
    LLVM_VERSION="$MCPP_E2E_LLVM_VERSION"
else
    # Version dirs only (e.g. 20.1.7, 22.1.8) — a payload root may contain
    # stray non-version entries.
    LLVM_VERSION="$(ls -1 "$_llvm_base" 2>/dev/null | grep -E '^[0-9]+(\.[0-9]+)*$' | sort -V | tail -1)"
fi
LLVM_ROOT="$_llvm_base/${LLVM_VERSION:-none}"
export LLVM_VERSION LLVM_ROOT
