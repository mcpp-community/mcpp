#!/bin/bash
# Phase 1 of a two-edge module compile: start the compiler, return as soon as the
# BMI is on disk, and leave codegen running in the background.
#
#   bmi_release.sh <slot> <bmi-path> -- <compiler> <args...>
#
# GCC writes the CMI to `<bmi>~` and rename()s it into place (verified by strace),
# so the file is atomically complete-or-absent: seeing it appear is sufficient —
# no stability polling, no locking.
#
# FAIRNESS: once the compiler is detached it is invisible to ninja's -j
# accounting, so live compilers would no longer be bounded by the job count and
# the A/B against the unsplit graph would be comparing different amounts of
# hardware. A counting semaphore (atomic `mkdir` tokens) is therefore held from
# compiler start to compiler exit, capping concurrent compilers at MCPP_BMI_JOBS.
# No deadlock is possible: a token holder never waits on another token.
set -u

SEMDIR="${MCPP_BMI_SEM:-.bmisem}"
JOBS="${MCPP_BMI_JOBS:-$(nproc)}"

sem_acquire() {
    mkdir -p "$SEMDIR"
    while :; do
        for i in $(seq 1 "$JOBS"); do
            if mkdir "$SEMDIR/$i" 2>/dev/null; then printf '%s' "$SEMDIR/$i"; return 0; fi
        done
        sleep 0.005
    done
}

slot="$1"; bmi="$2"; shift 2
[ "${1:-}" = "--" ] && shift

mkdir -p "$(dirname "$slot")"
rm -f "$slot.rc" "$bmi"

tok=$(sem_acquire)

# Detach: this script exits at BMI-flush while the compiler finishes codegen.
#
# THE TRAP: the background compiler inherits this process's stdout/stderr, which
# are ninja's pipes. ninja finishes an edge when the pipe reaches EOF, NOT when
# its direct child exits — so an inherited pipe makes the early exit completely
# invisible and every BMI edge is logged with the FULL compile duration. That is
# exactly what the first run of this prototype produced (BMI edges median 2018 ms
# == full compiles) and it read as "the idea does not work".
# Redirect the child's streams to a log; phase 2 replays it so diagnostics are
# still reported, attributed to the object edge.
( "$@" >"$slot.log" 2>&1 </dev/null
  rc=$?
  rmdir "$tok" 2>/dev/null
  echo "$rc" > "$slot.rc.tmp"; mv "$slot.rc.tmp" "$slot.rc" ) >/dev/null 2>&1 </dev/null &
echo $! > "$slot.pid"

while :; do
    [ -f "$bmi" ] && exit 0          # BMI landed -> importers may proceed
    if [ -f "$slot.rc" ]; then       # compiler finished without producing one
        rc=$(cat "$slot.rc")
        [ "$rc" = "0" ] && exit 0    # e.g. an implementation unit: no BMI by design
        exit "$rc"                   # real failure: fail this edge now
    fi
    sleep 0.005
done
