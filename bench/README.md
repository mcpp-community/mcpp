# `bench/` — build-engine benchmark suite

A cross-platform harness for measuring **build engines** against each other on
the **same C++ sources**, and for measuring what C++20 named modules actually
cost compared to headers.

Written in C++23 and built by mcpp, so it runs identically on Linux, macOS and
Windows — a shell-based harness cannot, and this suite replaced one that could
only run on Linux.

```bash
# generated fixtures, across engines and source forms
bench --engines mcpp,cmake,xmake,meson,bazel \
      --variants headers,modules,modules-impl \
      --scenarios cold,noop,touch-hub,edit-body \
      --compiler /path/to/g++ --jobs 32 --out report.json

# a REAL project, measured in place — e.g. mcpp building itself,
# comparing two mcpp binaries
bench --project . --engines mcpp=/usr/bin/mcpp,mcpp=./target/*/*/bin/mcpp \
      --scenarios noop,touch-hub --hub src/platform/platform.cppm
```

Each `mcpp=<path>` engine labels itself from the version that binary reports
(`mcpp@2026.8.12.1`), so two releases never collapse into one row. That is how
"did this release get faster?" is answered — by running both, not by emulating
one of them in the harness.

---

## 1. What is measured

**The build engine**, i.e. the graph it constructs and the order it schedules —
not the compiler, not package resolution, not download speed.

And, orthogonally, **the source form**: the same project emitted three ways.

| variant | shape | question it answers |
|---|---|---|
| `headers` | `unit_k.hpp` declares, `unit_k.cpp` defines | the status quo baseline |
| `modules` | `unit_k.cppm` declares **and** defines | what most module code looks like |
| `modules-impl` | `unit_k.cppm` declares, `unit_k_impl.cpp` defines | does splitting implementation out of the interface stop edit cascades? |

`modules-impl` exists because of a measured result: on **both** GCC 16.1 and
Clang 22.1 a module interface unit's BMI carries function bodies, so editing any
body changes the BMI and cascades to every importer. No compiler flag fixes it
(`-fmodules-reduced-bmi` was measured and does not). Moving bodies into
implementation units is the only available fix, and this variant is how that
claim gets a number instead of an argument.

---

## 2. Fairness invariants

| # | Invariant | How it is enforced |
|---|---|---|
| I1 | Identical compiler **binary** across engines | `--compiler <path>` is threaded into cmake (`-DCMAKE_CXX_COMPILER`), meson & xmake (`CXX`), bazel (`CC` + `--action_env`). mcpp uses its hermetic payload — a **declared asymmetry**, see §5. |
| I0 | Optimisations are measured, never emulated | Engines are parameterised by BINARY (`mcpp=<path>`). The harness contains no "what if we also set X" mode: emulating a change measures the harness's idea of it and silently stops tracking the implementation. |
| I2 | Identical source set | All variants come from one generator; no engine globs its own inputs. |
| I3 | Identical language level | C++23 everywhere; `import std;` is **absent from every fixture** (see §5). |
| I4 | Same parallelism | `--jobs N` is passed to every engine that accepts one. |
| I5 | A failure can never look like a measurement | `status` and timings are separate protocol fields; a non-ok cell carries **no** median. |
| I6 | A skip carries its reason | `unavailable` (not installed / cannot build this variant) is distinct from `failed`, and both require a note. |

---

## 2b. Two modes

| mode | fixture | when |
|---|---|---|
| **generated** (default) | `--units/--fanin/--weight` synthesise the same project in three source forms | comparing **source forms**, and engines against each other on identical input |
| **project** (`--project DIR`) | an existing tree, measured **in place** | comparing **engine binaries** on a real codebase — mcpp building itself is the base case |

### Measure a PINNED SNAPSHOT, never your working tree

```bash
BASE=$(git merge-base origin/main HEAD)
mkdir -p ~/.local/share/mcpp-bench-src/mcpp
git archive "$BASE" | tar -x -C ~/.local/share/mcpp-bench-src/mcpp

bench --project ~/.local/share/mcpp-bench-src/mcpp \
      --engines mcpp=/path/to/old,mcpp=/path/to/new \
      --scenarios cold --hub src/platform/platform.cppm
```

Benchmarking the tree you are editing does not merely add noise — it produces
**wrong results that look real**. Measured here: a job-count sweep reported
`rc=1` at three different job counts in a row, which read as "the design fails
above 16 concurrent compiles". The actual cause was that a new module had been
added to the working tree between generating `build.ninja` and running the
sweep, so every arm was building a source set its graph did not know about. A
snapshot pinned to a commit cannot drift underneath a measurement.

A plain `git archive` (not a clone or worktree) is deliberate: no `.git`, no
shared state, nothing that a branch switch in the real repo can reach.

In project mode the variant axis collapses to `native`: the project is whatever
it already is, and generating over it would destroy the thing being measured.
Scenarios that perturb a file need to be told which one (`--hub`, `--leaf`,
`--body`); without it they report `skipped` **with the reason** rather than
picking a file and producing a number that looks valid.

`edit-body` rewrites a source file. In project mode that file belongs to the
user, so its exact bytes are captured before and restored afterwards — including
when the build fails, which is precisely when a leftover edit would be missed.

---

## 3. Scenarios

| Scenario | Perturbation | What it exercises |
|---|---|---|
| `cold` | `clean()`, then time **configure + build** | full graph construction + every compile |
| `noop` | nothing | the up-to-date check / fast path |
| `touch-hub` | mtime bump on the most-depended-on unit, **content unchanged** | can the engine prove the interface did not change and stop the cascade? |
| `edit-body` | insert a **numbered** marker inside a function body, interface untouched | the everyday developer loop |
| `touch-leaf` | mtime bump on a unit nobody depends on | recompile 1 + link |

Two details that are easy to get wrong and change the answer:

* **`cold` includes configure.** cmake and meson keep configure output inside the
  build directory that `clean` removes, so building without re-configuring simply
  fails. Timing configure separately would also be wrong: the user waits for both,
  and engines that fold configure into the build (mcpp, bazel) would get a
  discount for it.
* **`edit-body` uses a counter.** An idempotent edit is a real edit on run 1 and a
  bare `touch` on runs 2..N — a different, much cheaper scenario, silently
  dragging the median toward it.

---

## 4. Statistical method

* Medians, with min/max. No confidence intervals: sample counts are small by
  necessity and a computed interval would imply more rigour than exists.
* `cold` defaults to 3 runs, incremental scenarios to 5 (`--runs` overrides).
* One **untimed seed build** per cell: an incremental scenario is only incremental
  against an up-to-date tree, and it warms the page cache so run 1 is not
  systematically slower.
* Page cache is deliberately left **warm**. A cold-page-cache build is not a
  situation developers live in, and dropping caches adds variance unrelated to
  the engine.
* The harness never lets build output reach its own stdout; child streams go to
  `<work>/logs/<engine>-<scenario>.log`. A mixed stream cannot be parsed — and
  the log lives under the WORK root, never inside the measured tree, so a
  `--project` run cannot drop scratch into someone's repository.

---

## 5. Declared asymmetries

These cannot be removed, so they are stated rather than hidden.

* **mcpp uses its own hermetic toolchain.** `--compiler` pins the others; mcpp
  resolves gcc/llvm from its registry by design. Point `--compiler` at that same
  payload (`~/.mcpp/registry/data/xpkgs/xim-x-gcc/<ver>/bin/g++`) to close the gap.
* **No fixture says `import std;`.** Engines differ wildly in how — and whether —
  they can build the std module (CMake needs a per-version experimental UUID,
  meson has no story). That difference would dominate every measurement. The
  fixtures reach the standard library through the global module fragment, which
  every engine handles identically. **This suite measures module machinery, not
  std-module support.**
* **bazel's cold is not a cold machine.** It keeps a warm server and an action
  cache outside the workspace. `clean` here is deliberately *not* `--expunge`,
  which would also discard the toolchain and turn the measurement into
  provisioning. Every bazel cell says so in its note.
* **meson and bazel are headers-only.** Their C++20 named-module support is not
  comparable to cmake's or xmake's; they report `unavailable` with a reason
  rather than producing a number that does not mean what it looks like.

---

## 6. Result protocol

Results are JSON, versioned by `protocol_version` (currently **1**). Any field
addition, removal or semantic change bumps it.

```json
{
  "protocol_version": 1,
  "started_at": "2026-08-12T12:04:42Z",
  "host": { "os": "linux", "arch": "x86_64", "cpu_model": "...",
            "logical_cores": 32, "physical_cores": 24,
            "heterogeneous": true, "ram_bytes": 67147722752, "toolchain": "..." },
  "cells": [ { "engine": "mcpp", "compiler": "gcc", "profile": "release",
               "scenario": "cold", "fixture": "synth-40x3", "variant": "modules",
               "status": "ok", "note": "...", "runs": 3,
               "median_s": 12.345, "min_s": 12.100, "max_s": 12.600,
               "samples": [12.1, 12.345, 12.6] } ]
}
```

`heterogeneous` is not decoration: on a 13900K, "32 cores" is 8 P-cores + 16
E-cores, and every average-parallelism figure has to be read against that.

A non-ok cell has **no timing keys at all** rather than zeros — a reader that
forgets to check `status` gets a missing key (loud) instead of a `0.0` (silent).

---

## 7. Extending

**Adding an engine** is one new module implementing `bench::engines::Engine` plus
one line in `registry.cppm`. The runner, protocol, scenarios and CI do not change.

**Adding a scenario** is one enum value in `spec.cppm` plus one case in
`Runner::perturb`.

**Platform work** goes in `src/platform/{posix,windows}.cppm`. Each guards its
whole body with a single macro and exports the same names, so exactly one
definition exists per build and the compiler selects it — no stubs, no dispatch.
`#if defined(_WIN32)` appears in those two files and nowhere else in the suite.
(Same convention as xlings' `src/platform/*.cppm`.)

---

## 8. Analysis mode

The same binary profiles an existing ninja build directory:

```
bench --analyze target/x86_64-linux-gnu/<fingerprint>
```

reporting work, makespan, **critical path** and the concurrency profile. The
number to read first is the critical path as a percentage of makespan: at ~100%
the build is latency-bound and more cores buy nothing.

Five parsing traps it exists to get right — each one changed a conclusion during
the original analysis:

1. A multi-output edge (`build a.o | a.gcm : cxx_module`) writes **one
   `.ninja_log` line per output**, sharing start/end. Summing lines double-counts
   compile time (302 s reads as 604 s).
2. For a modules build the real edges live in the **dyndep files**
   (`obj/*.ddi.dd`), not in `build.ninja`. Ignoring them made mcpp's critical path
   measure 22 s instead of 79 s.
3. dyndep attaches deps to `obj/X.m.o`, but importers depend on the *other* output
   of that edge, `gcm.cache/X.gcm`. Unless every output of an edge is one graph
   node, the longest-path walk terminates after two hops.
4. ninja **appends** to `.ninja_log` and restarts its clock each invocation. A log
   touched by several builds mixes overlapping ranges; the tell is a critical path
   **above 100% of makespan** (xlings' log first read as 136%).
5. Longest path must be relaxed in **topological order**. A stack DFS's
   "skip what is on the stack" cycle guard also skips a dependency a sibling
   pushed but has not finished, scoring it 0 — reported **33.9 s over 10 nodes**
   where the truth is **76.5 s over 26**, turning a 100%-critical-path build into
   a 44% one and inverting the diagnosis.

> **Cross-check anything that computes a critical path.** Every other metric
> agreed between two independent implementations while this one was wrong by 2.3x.

---

## 9. The `xmake.lua` at the repository root

Separate from the generated fixtures, the repo root carries an `xmake.lua` that
builds **mcpp itself** — the control arm for "same real project, different
engine". Synthetic fixtures cannot reproduce the dependency shape of a real
137-module codebase, so both exist.

It pins the compiler by reading `[toolchain] default` out of `mcpp.toml`, because
the registry holds several GCCs and "newest directory wins" only *happens* to
agree with the pin. Verify before quoting anything from it:

```bash
xmake f -y -m release --toolchain=mcpp-gcc
xmake show -t mcpp | grep 'compiler (cxx)'     # must be the same binary mcpp uses
```

> An earlier revision called `set_toolchains()` unconditionally, which silently
> overrode `xmake f --toolchain=llvm`: the "clang" cell was in fact compiled by
> g++, and the only tell was that its number landed within noise of the gcc cell.
> That check above is not ceremony.

## 10. Running

```bash
cd bench && mcpp build
./target/*/*/bin/bench --list                 # what is installed here
./target/*/*/bin/bench --units 40 --fanin 3   # default matrix
```

CI: `.github/workflows/bench.yml`, **manual trigger only**
(`workflow_dispatch`). A benchmark is heavy and noisy; attaching it to every PR
would drown the signal, and no threshold assertion is made — host variance
(heterogeneous CPUs, cloud neighbours) is larger than most real regressions.

## 11. Host record

A result is only meaningful next to its host, and the report carries it
automatically. When quoting numbers by hand, quote the CPU model, whether it is
**heterogeneous**, thread count, RAM, compiler version and engine versions too.
