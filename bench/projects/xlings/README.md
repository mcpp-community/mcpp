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

## The cmake description, and where it stops

`CMakeLists.txt` here is real — it configures, finds all 110 module interface
units, and compiles them. **It does not link**, and the reason is worth having
written down, because it is the honest limit of a hand-written foreign build
description rather than a gap in effort:

The dependencies themselves describe fine: every header they need is unpacked
in mcpp's registry, and `CMakeLists.txt` finds all of them **transitively** —
`mbedtls` arrives through mcpplibs `tinyhttps`, `lua` through `capi.lua`.

The blocker is one level past that, and it is specific:

```
xpkg-executor.cppm:5  fatal error: unknown compiled module interface: no such module
                      [mcpplibs.xpkg.lua_stdlib]
```

`mcpplibs.xpkg.lua_stdlib` **is not a checked-in file**. That package generates
it at build time with a `build.mcpp` program — mcpp's build-program protocol. No
foreign build system can produce it without implementing that protocol, so this
is not a gap in the description; it is the boundary of what a description can be.

**So the xlings arm compares mcpp against mcpp** (two releases, or two
schedules). That is what a control target is for: it answers *"does this engine
change hold on a codebase nobody tuned it for?"*, and that question does not
need a second engine. The cross-engine arm stays on
[`../mcpp/`](../mcpp/), which has one source dependency and lives in this
repository, so its descriptions can be kept correct.
