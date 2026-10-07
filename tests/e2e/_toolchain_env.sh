#!/usr/bin/env bash
# _toolchain_env.sh — THE single place an e2e test learns a toolchain version.
#
# A fixture that means "the llvm row" must not inline `llvm@23.1.3`. The line
# moves (SPEC-009 §10); when it did, ~90 literals across 38 scripts turned
# every move into a sweep, and one grep escaped the sweep anyway (804, found
# in #781's CI). Source this file and use the variables instead:
#
#     source "$(dirname "$0")/_toolchain_env.sh"
#     printf '[toolchain]\nmacos = "llvm@%s"\n' "$LLVM_VERSION" > mcpp.toml
#     "$MCPP" build --toolchain "llvm@${LLVM_VERSION}"
#
# Per family the version resolves in three steps:
#   1. $MCPP_E2E_<FAMILY>_VERSION — an explicit override, for a leg probing
#      one release against the whole suite;
#   2. the newest installed payload of the family in the registry mcpp uses —
#      what CI prewarmed and what the run will actually resolve;
#   3. the fallback constant at the bottom of this file — the engine's
#      current line, and THE ONLY EDIT a line move requires here.
#
# Variables (a version is never empty: with no override and no installed
# payload the fallback stands, which is what lets a fixture pin a toolchain
# the engine then installs on first use):
#   LLVM_VERSION / LLVM_ROOT                 store dir xim-x-llvm
#   GCC_VERSION / MCPP_E2E_GCC_ROOT          store dir xim-x-gcc
#   MUSL_GCC_VERSION / MUSL_GCC_ROOT         store dir xim-x-musl-gcc
#   MINGW_CROSS_VERSION / MINGW_CROSS_ROOT   store dir xim-x-mingw-cross-gcc
#
# The gcc-family sweeps have not been done yet: the llvm family is the line
# that moves, and its scripts are migrated. Migrating a family is mechanical
# — replace its literals with the variable, source this file — and worth
# doing in the PR that next touches that family's tests.
#
# Usage:   source "$(dirname "$0")/_toolchain_env.sh"

_e2e_registry_base="${MCPP_HOME:-${HOME}/.mcpp}/registry/data/xpkgs"
if [[ -z "${MCPP_HOME:-}" && ! -d "$_e2e_registry_base" && -n "${USERPROFILE:-}" ]]; then
    _e2e_registry_base="${USERPROFILE}/.mcpp/registry/data/xpkgs"
fi

# _e2e_family_version <store-dir> <override-var-name> <fallback>
_e2e_family_version() {
    local override="${!2:-}"
    if [[ -n "$override" ]]; then
        printf '%s\n' "$override"
        return
    fi
    # Version dirs only (e.g. 20.1.7, 22.1.8) — a payload root may contain
    # stray non-version entries.
    local installed
    installed="$(ls -1 "$_e2e_registry_base/$1" 2>/dev/null \
        | grep -E '^[0-9]+(\.[0-9]+)*$' | sort -V | tail -1)"
    printf '%s\n' "${installed:-$3}"
}

_e2e_family_root() {
    printf '%s\n' "$_e2e_registry_base/$1/$2"
}

LLVM_VERSION="$(_e2e_family_version xim-x-llvm MCPP_E2E_LLVM_VERSION 23.1.3)"
LLVM_ROOT="$(_e2e_family_root xim-x-llvm "$LLVM_VERSION")"

GCC_VERSION="$(_e2e_family_version xim-x-gcc MCPP_E2E_GCC_VERSION 16.1.0)"
# GCC_ROOT is a compiler control variable: GCC uses it to rewrite executable
# and library prefixes. A fixture's registry path must not alter the driver's
# lookup, especially after a test switches to a cold MCPP_HOME.
MCPP_E2E_GCC_ROOT="$(_e2e_family_root xim-x-gcc "$GCC_VERSION")"

MUSL_GCC_VERSION="$(_e2e_family_version xim-x-musl-gcc MCPP_E2E_MUSL_GCC_VERSION 15.1.0)"
MUSL_GCC_ROOT="$(_e2e_family_root xim-x-musl-gcc "$MUSL_GCC_VERSION")"

MINGW_CROSS_VERSION="$(_e2e_family_version xim-x-mingw-cross-gcc MCPP_E2E_MINGW_CROSS_VERSION 16.1.0)"
MINGW_CROSS_ROOT="$(_e2e_family_root xim-x-mingw-cross-gcc "$MINGW_CROSS_VERSION")"

export LLVM_VERSION LLVM_ROOT GCC_VERSION MCPP_E2E_GCC_ROOT \
       MUSL_GCC_VERSION MUSL_GCC_ROOT MINGW_CROSS_VERSION MINGW_CROSS_ROOT
