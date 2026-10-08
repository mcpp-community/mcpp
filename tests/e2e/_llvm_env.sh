#!/usr/bin/env bash
# _llvm_env.sh — the LLVM slice of _toolchain_env.sh, kept as an alias for the
# scripts that source it by this name. New scripts source _toolchain_env.sh
# directly; it sets LLVM_VERSION and LLVM_ROOT with the same semantics this
# file always had, plus the other families.

source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
