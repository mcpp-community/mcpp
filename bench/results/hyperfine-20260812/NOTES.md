# Result provenance

> **These files predate protocol v1.** They were produced by the one-off bash +
> hyperfine harness that `bench/` replaced, and are kept as reference data for the
> 2026-08-12 analysis — the TSVs have no `status` column, which is precisely the
> gap that let a failed cell be written as `0.000` (see below). New runs emit the
> versioned JSON described in `bench/README.md` §6; do not merge the two formats.

Raw `hyperfine` JSON and the per-run TSV land here. Read this before quoting a
number out of them.

## Host (all runs below)

| | |
|---|---|
| CPU | Intel i9-13900K — **8 P-core + 16 E-core, 32 threads** (heterogeneous: do not read "32 cores" as 32 equal cores) |
| RAM | 62 GB |
| Kernel | Linux 6.8 |
| Compiler | GCC 16.1.0, mcpp hermetic payload (`~/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0`) |
| Engines | mcpp 2026.8.11.3 · xmake v3.0.7+HEAD.77d94ad |
| Project | mcpp itself — 137 `.cppm` + 1 `.cpp`, 56 555 LOC |

## `matrix-20260812-104244.tsv`

Valid: all five `mcpp` rows, plus `xmake` `cold`, `noop`, `touch-hub`.

**Void: the `xmake` `edit-body` and `touch-main` rows.** They read `0.000`, which
is not a measurement — the build failed on every run and an early version of
`run.sh` tested `[[ -f json ]]` instead of `[[ -s json ]]`, so an empty
hyperfine export was formatted as a zero. The cause was self-inflicted: `xmake.lua`
was edited *while the matrix was running*, and the edit read a file from xmake's
description scope, where `io` is nil (`attempt to index a nil value (global 'io')`).
Both bugs are fixed — `run.sh` now records `FAILED`, and `xmake.lua` reads the
manifest inside `on_load`. Those two cells were re-measured; see the newer TSV.

Two lessons worth keeping:
1. Never edit the build description of a benchmark that is mid-flight.
2. A benchmark harness must not be able to emit a number when the thing it was
   timing did not run.

## `matrix-20260812-112142.tsv`

Valid: `mcpp / clang / release / cold` = **32.076 s**.

**Void: the `xmake / clang / release / cold` row (90.233 s).** `xmake.lua` called
`set_toolchains("mcpp-gcc")` unconditionally, which silently overrode
`xmake f --toolchain=llvm`; that cell was compiled by **g++**, not clang. The tell
was that it landed within noise of the gcc cell (88.942 s). Fixed: the pin is now
skipped when the caller requested a toolchain. Always confirm with

```bash
xmake show -t mcpp | grep 'compiler (cxx)'
```

With the override fixed, xmake *does* select `clang++ 22.1.8` — but the build then
fails outright:

```
error: <mcpp> missing std dependency for module mcpp.cli.cmd_build
warning: std and std.compat modules not found! maybe try to add --sdk=<PATH/TO/LLVM>
```

even with `--sdk=<mcpp llvm payload>`, and even though that payload does ship
`lib/x86_64-unknown-linux-gnu/libc++.modules.json` and
`share/libc++/v1/std.cppm`. xmake v3.0.7 does not discover libc++'s std module
from this layout. mcpp does not depend on that discovery — it precompiles
`std.pcm` itself. **The xmake/clang cell is therefore unmeasured, not slow.**

## Declared asymmetry: the `std` module

mcpp stages a prebuilt `std.gcm` (31.5 MB) out of `~/.mcpp/build-cache/v1/`;
xmake compiles `std` from libstdc++ sources. Both end up with byte-comparable
artifacts (31 458 736 B vs 31 458 752 B). Because **every** module imports `std`,
that compile sits on xmake's critical path and mcpp's cold-build advantage is
partly a *caching* advantage, not a *scheduling* one. See the analysis doc for
the measured size of that head start before attributing the cold-build delta.
