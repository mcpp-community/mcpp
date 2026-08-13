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

## Pinned as submodules — and why that replaced "not vendored"

This directory used to say *"there is no copy of xlings here, on purpose: a
vendored snapshot rots"*, and CI cloned the default branch at run time.

**The reasoning was right and the implementation did the opposite of it.** A
target cloned from a moving branch does not merely rot, it rots *invisibly*:
`--hub src/xlings.cppm` went on naming a file that had stopped existing, so
every xlings cell reported `skipped`, every xlings job reported success, and
nobody had a reason to look. Drift was not prevented — it was made unobservable.

A submodule is a **pin**, not a snapshot. The commit is in the diff, it is
reviewed like any other change, bumping it is a deliberate act with a
before/after, and `tests/e2e/233_bench_matrix.sh` can check that each `hub` and
`body` still exists in the tree CI will actually measure.

```bash
git submodule update --init          # get both pinned trees
```

| directory | version | commit | shape | variant |
|---|---|---|---|---|
| `xlings-2026.8.11.2` | 2026.8.11.2 | `b1563fe` | 110 `.cppm` + **2** `.cpp` | `modules` |
| `xlings-2026.8.13.1` | 2026.8.13.1 | `f072075` | 110 `.cppm` + **92** `.cpp` | `modules-impl` |

### Two pins, because the code style is the measurement

They are the same project either side of one refactor — `f072075` moved the
implementations out of the interface units. Same module graph, same 46k lines,
opposite answers to "where does the code live". That is the `modules` vs
`modules-impl` axis the generated fixture has, except here it was done by people
who were not thinking about this benchmark, which is the entire value of it.

`--body` follows the style: the `.cpp` in the split tree, the `.cppm` in the
combined one. Editing an implementation is the point, and in the combined style
the implementation *is* the interface unit — which is why the two are expected
to behave differently, and why measuring both is the only way to say by how much.

**One description serves both.** `CMakeLists.txt` and `xmake.lua` here glob
`src/**/*.{cppm,cpp}` — the same rule mcpp infers from — so neither style needs
its own file, an environment switch, or a branch. They used to name
`src/main.cpp` alone, which against the split tree compiles 110 interfaces,
links nothing, and still reports a time.

```bash
bench --project bench/projects/xlings/xlings-2026.8.13.1 \
      --buildfiles bench/projects/xlings \
      --engines mcpp=<new>,mcpp --compiler payload:gcc \
      --scenarios cold,noop --hub src/platform.cppm --body src/platform.cpp
```

`--hub src/platform.cppm` is the hub because it has the most importers (45 in
the combined tree, 54 in the split one).

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

## The foreign build descriptions, and where each stops

| engine | file | status |
|---|---|---|
| cmake | [`CMakeLists.txt`](CMakeLists.txt) | configures, compiles all 110 units; **does not link** |
| xmake | [`xmake.lua`](xmake.lua) | same shape, same gap; shares the toolchain definitions in [`../common/xmake/payload.lua`](../common/xmake/payload.lua) |
| bazel | [`MODULE.bazel`](MODULE.bazel) | **cannot** — the workspace boundary, and this tree is not even in the repository |
| meson | — | removed from the suite entirely: meson cannot declare a module interface unit at all (see `../../SPEC.md`) |

Both working arms take the compiler as a parameter, so the **toolchain is a real
axis here** and not a label: gcc gets `-B<binutils>` + `--sysroot`, clang gets its
own include chain (handing clang gcc's sysroot puts the two arms on different
libc), msvc gets nothing because mcpp uses the system Visual Studio too. That
logic is shared with the mcpp arm rather than copied — see
[`../common/`](../common/).

## Where the cmake and xmake arms stop

`CMakeLists.txt` here is real — it configures, finds all 110 module interface
units, and compiles them. **It does not link**, and the reason is worth having
written down, because it is the honest limit of a hand-written foreign build
description rather than a gap in effort:

**82 of 83 edges: every translation unit compiles; only the link fails.**

Two things looked like boundaries and were not:

* **Transitive headers.** All of them are unpacked in mcpp's registry and
  `CMakeLists.txt` finds them — `mbedtls` via mcpplibs `tinyhttps`, `lua` via
  `capi.lua`.
* **A generated module.** `mcpplibs.xpkg.lua_stdlib` is not checked in; libxpkg's
  `build.mcpp` produces it. But all it does is embed eleven `.lua` files as
  strings, so [`embed_lua_stdlib.cmake`](embed_lua_stdlib.cmake) reproduces it.
  *"mcpp runs a build program"* is not by itself a boundary.

What is left is ordinary work rather than a wall: `ftxui`, `libarchive`, `lua`
and `mbedtls` arrive as **source** and mcpp compiles them, so the link asks for
symbols nobody built here (`undefined reference to archive_entry_pathname`, …).
Each ships its own CMakeLists, so `add_subdirectory` finishes the arm.

⚠️ The copied module list in the generator **already drifted once**: a first
regex caught ten of eleven entries, and the failure surfaced three files away as
`error: 'base64_lua' is not a member of ...detail`. The generator now fails on a
missing `.lua` rather than trusting the list.

**So the xlings arm compares mcpp against mcpp** (two releases, or two
schedules). That is what a control target is for: it answers *"does this engine
change hold on a codebase nobody tuned it for?"*, and that question does not
need a second engine. The cross-engine arm stays on
[`../mcpp/`](../mcpp/), which has one source dependency and lives in this
repository, so its descriptions can be kept correct.
