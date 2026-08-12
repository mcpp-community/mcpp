# Prototype: release importers at BMI-flush, not at compiler exit

A throwaway, measurable prototype of the largest optimisation identified in
`.agents/docs/2026-08-12-modular-build-performance-deep-analysis.md`. It is not
production code — it exists so the proposal rests on a measurement instead of a
simulation, and so the two implementation hazards below are on record before
anyone builds the real thing.

## What it does

`split_graph.py` mechanically rewrites mcpp's generated `build.ninja` so each
module interface unit becomes two edges driven by **one** compiler process:

```
build gcm.cache/X.gcm : cxx_module_bmi src/X.cppm | X.ddi.dd   # exits at BMI rename
build obj/X.m.o       : cxx_module_obj gcm.cache/X.gcm         # waits for codegen
```

Importers already depend on `gcm.cache/X.gcm` (dyndep emits exactly that), so
nothing downstream needs rewriting — those dependencies simply become satisfiable
about 4x earlier. The link edge still waits for every object.

## Measured on this repo

```
baseline (edge-complete release)  77.42 s
split    (BMI-flush release)      36.56 s     2.12x, identical 19,347,008 B binary
```

Both arms cap concurrent compilers at `nproc`. Total CPU work is unchanged.

```bash
bench/proto-bmi-release/run_proto.sh
```

## Two hazards this prototype exists to document

**1. The detached compiler must not inherit the build system's stdout/stderr.**
ninja ends an edge at pipe EOF, not at direct-child exit. Leave the pipe
inherited and the early exit is invisible: every BMI edge logs the *full* compile
duration and the arm silently measures the baseline. The first run here did
exactly that (BMI edges median 2018 ms == full compiles) and looked like "the
idea does not work". Redirect the child's streams to a file and replay them from
phase 2, or compiler diagnostics vanish.

**2. ninja's `-j` must be far larger than the compiler cap.**
Once detached, a compiler no longer holds a ninja slot, so concurrency is bounded
by the semaphore instead. With `-j` equal to the cap, ninja's slots fill with
edges that are only sleeping — blocked on the semaphore or waiting for codegen —
and the ready frontier starves. The first run used `-j32` with a cap of 32 and
came out at 78.99 s, *slower* than baseline. The measurement above uses `-j` = 6x
the cap.

## Known limitations (fine for cold-build timing, not for production)

- Drops the `-MMD` depfile plumbing, so header dependencies of global module
  fragments are not tracked → valid for cold builds, not incremental correctness.
- Polls the filesystem every 5 ms and implements the semaphore with `mkdir`
  tokens; a real implementation should use the GCC module-mapper protocol
  (`MODULE-COMPILED` is the ready signal) or proper job control.
- No cleanup of detached compilers on SIGINT. Production needs process groups —
  on Windows, a Job Object.
