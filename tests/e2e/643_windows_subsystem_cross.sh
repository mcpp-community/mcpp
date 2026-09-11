#!/usr/bin/env bash
# requires: mingw-cross
# 643_windows_subsystem_cross.sh -- `windows_subsystem` / `windows_entry` (#618)
# from Linux to Windows through mingw-cross, which links for the GNU ABI
# (`-mwindows`, `-municode`). The assertions are shared with 642. The execution
# leg runs under wine when this host has it; wine is evidence rather than proof
# (see 257), and the native half is 642.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="${MCPP_HOME:-$HOME/.mcpp}"
export WINEDEBUG=-all

BUILD_ARGS="--target x86_64-windows-gnu"
RUN_EXE=""
command -v wine > /dev/null 2>&1 && RUN_EXE="wine"
source "$(dirname "$0")/_windows_subsystem_body.sh"
