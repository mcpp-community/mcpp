#!/bin/bash
# Phase 2 of a two-edge module compile: block until the compiler started by
# bmi_release.sh has finished, and propagate its exit status.
#
#   bmi_wait.sh <slot> <object-path>
#
# This edge holds a ninja job slot while it waits, which is deliberate: it keeps
# the number of live compilers bounded by -j even though the compiler is no
# longer this script's child.
set -u
slot="$1"; obj="${2:-}"

while [ ! -f "$slot.rc" ]; do sleep 0.01; done
rc=$(cat "$slot.rc")
# Replay whatever the detached compiler said. Its streams were redirected to a
# file so that phase 1 could exit early without holding ninja's pipe open; if
# they were not replayed here, warnings and errors would vanish silently.
[ -s "$slot.log" ] && cat "$slot.log" >&2
if [ "$rc" != "0" ]; then exit "$rc"; fi
if [ -n "$obj" ] && [ ! -f "$obj" ]; then
    echo "bmi_wait: compiler reported success but $obj is missing" >&2
    exit 1
fi
exit 0
