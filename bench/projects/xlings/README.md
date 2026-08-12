# `xlings` — the independent control target

mcpp measuring its own build proves nothing about **build performance in
general**: an optimisation can be an artefact of one project's module graph, and
"make the benchmark's target faster" is not an optimisation at all. A second
project, written by different people against a different structure, is what
separates the two.

`xlings` fits: **110 module interface units, 46k lines**, every one of them
`import std;`, and it already carries an `mcpp.toml` — so mcpp builds it with no
adaptation, which is exactly what makes it a fair control rather than a
purpose-built fixture.

## Not vendored, on purpose

There is no copy of xlings here. A vendored snapshot rots, and a benchmark whose
target silently drifts from the real project measures the snapshot. Point the
harness at a checkout instead:

```bash
git clone https://github.com/openxlings/xlings   # any recent commit
bench --project /path/to/xlings --engines mcpp=<old>,mcpp=<new> \
      --scenarios cold,noop --runs 2
```

Record the commit with the numbers. The measurements below are from
**`b1563fe`**.

## What it has shown so far

The split module schedule (`schedule = "on"`, see
`.agents/docs/2026-08-13-build-performance-architecture.md` L2) reproduces on
both projects, with a *larger* effect on the one that was not used to develop it:

| project | modules / lines | `schedule=off` | `schedule=on` | ratio |
|---|---|---|---|---|
| mcpp | 138 / 57k | 79.9s | **34.80s** | **2.30x** |
| **xlings** | 110 / 46k | 112.92s | **33.41s** | **3.38x** |

Both are no-ops on a second build — mcpp 0.21s, xlings 10.77s where the whole
10.77s is dependency resolution and `.ninja_log` grows by **zero edges**. That
distinction matters: a schedule whose depfile target is wrong looks exactly like
success while recompiling everything, and counting re-run edges is the only
check that catches it.

**Declared asymmetry**: in the run above, the `off` arm compiled
`mcpplibs.xpkg` (5 units) while the `on` arm hit the dependency cache. Five
units against an 80-second difference does not move the conclusion, but it is
recorded rather than smoothed over.

## Why there are no cmake/xmake descriptions here

`bench/projects/mcpp/` carries them because mcpp is the project this repository
can keep them correct for. Writing them for someone else's tree means owning a
build description that must track a codebase we do not control — it would be
stale on the first upstream refactor, and a stale description does not fail, it
just measures something else. The xlings arm therefore compares **mcpp against
mcpp** (releases, or schedules), which is what a control target is for.
