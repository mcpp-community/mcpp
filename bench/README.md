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

`modules-impl` gives the "move bodies out of interface units" advice a number.
What that number is turns out to depend on the compiler, and an earlier version
of this paragraph asserted the **opposite** of the measurement:

* GCC 16.1 does **not** put the body of an exported non-template function into
  the BMI. Editing such a body changes the object file and leaves the BMI
  byte-identical apart from its embedded timestamps.
* So the cascade other engines pay for that edit is avoidable, and the engines
  split by their *decision rule*: compare the BMI's **content** (mcpp, 0.3 s) or
  trust its **mtime** (cmake and xmake, ~10 s).
* Templates and inline functions in an interface unit **do** change the BMI. The
  advice survives; its justification is narrower than it was written to be.

Establishing this needs a control — compile the *same* source twice and diff the
BMIs. The differing bytes land at the same offsets either way, inside
`buildtime:`/`localtime:`. Without that control the timestamp reads as a content
change and the conclusion inverts.

### 1a. The workload must actually be the workload

A size knob that does not move the cost is worse than no knob: it makes a
benchmark look tunable while it measures something else. The first version of
this fixture failed exactly there.

| | cost per unit (gcc 16.1, x86_64) |
|---|---|
| empty module | 0.17 s |
| **old fixture unit, `weight 6`** | **0.23 s** — 74% of it compiler startup |
| old fixture unit, `weight 40` | 0.28 s — a 6.7x knob bought 20% |
| one unit with a realistic global module fragment | 0.97 s |
| **mcpp's own units** (57k lines / 139 units) | **0.57 s** |

The old `weight` emitted O(weight²) instantiations of one trivial `constexpr`
recursion — a few hundred at weight 40, which a compiler does in microseconds.
Unit *count* scaled cost linearly at 0.088 s each; `weight` did not scale it at
all. **The suite was largely measuring `g++` starting up.**

The workload is now built from what actually costs time in real C++: standard
library headers, plus instantiation over **distinct types** so blocks cannot
share instantiations. Cost is `0.38 s + 0.066 s × weight`, and the knob is
verified to move: at 20 units, `weight` 0 / 4 / 12 gives 4.7 s / 18.0 s / 31.4 s
cold.

**Rule.** Any future knob must come with a measured sweep showing it changes
cost, in this file. A knob without one is assumed inert.

### 1b. Named sizes

A benchmark whose size is a free-form triple of numbers cannot be compared
between two people. `--preset` names it, and the default shape **is** `standard`
so that "no flags" and `--preset standard` cannot mean different things.

| preset | units | fan-in | weight | mcpp cold (gcc, modules) |
|---|---|---|---|---|
| `smoke` | 4 | 2 | 1 | ~2 s — CI and the e2e test, not for publication |
| `standard` | 20 | 3 | 4 | ~18 s — what published results use |
| `large` | 60 | 3 | 6 | minutes |

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
| `edit-comment` | insert a **comment** into the most-depended-on unit — bytes change, interface does not | mtime is no longer enough; only comparing the produced BMI avoids the cascade |
| `edit-body` | insert a **numbered `volatile` statement** into a function body | the everyday developer loop: real codegen change, interface untouched |
| `touch-leaf` | mtime bump on a unit nobody depends on | recompile 1 + link |

Two details that are easy to get wrong and change the answer:

* **`cold` includes configure.** cmake and meson keep configure output inside the
  build directory that `clean` removes, so building without re-configuring simply
  fails. Timing configure separately would also be wrong: the user waits for both,
  and engines that fold configure into the build (mcpp, bazel) would get a
  discount for it.
* **`edit-body` uses a counter**, and the counter is in the *identifier*. An
  idempotent edit is a real edit on run 1 and a bare `touch` on runs 2..N — a
  different, much cheaper scenario, silently dragging the median toward it. The
  inserted statement is `volatile`, so no optimiser can delete it and hand back
  the previous object file, and its name carries the nonce, because
  perturbations ACCUMULATE within a cell and a fixed name redeclares itself on
  run 2.
* **`edit-body` and `edit-comment` are separate on purpose.** They were one
  scenario, named `edit-body`, that inserted a comment — so every "N times
  faster on edits" number it produced was really a statement about comments.
  Splitting them costs one extra column and makes each number mean its name.

  On GCC 16.1 both happen to be cheap for the same underlying reason, and it is
  worth stating because it is easy to misread as a bug: **GCC does not encode
  the body of an exported non-template function into the BMI.** Editing such a
  body changes the object file and leaves the BMI byte-identical apart from its
  embedded `buildtime:`/`localtime:` stamps, so skipping the importers is
  correct, not a missed rebuild. Establishing that requires a control — compile
  the *same* source twice and diff: the differing bytes land at the same offsets,
  inside the timestamps.

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

### 4a. Validity rules — when a cell must NOT be compared

Every result file carries `median_s`, `min_s`, `max_s` and every raw `sample`.
Two rules decide whether a number means anything, and both are computable from
those fields alone — no trust in the harness required.

**R1 — resolution.** Each engine's `noop` row for the same variant is its floor:
what it costs to ask "is anything out of date?" before any work happens. A cell
within **2x of its own engine's `noop`** is measuring process startup and
bookkeeping, not building, and must not be read as a build comparison.

> This is why the `headers` rows read the way they do at small sizes. With the
> old fixture, `cmake` `noop` was 0.33 s and `cmake` `edit-body` was 0.79 s —
> 2.4x, right at the edge. The three fastest engines sat inside a 0.15 s band
> that is *entirely* startup. Those cells were never a ranking.

**R2 — dispersion.** If `(max_s − min_s) / median_s > 0.20`, the cell is noisy
and only order-of-magnitude claims survive it. Report it, do not silently
re-run: a cell that needs re-running to look stable is a cell whose number
depends on the machine's mood.

Neither rule is applied automatically. Automatic suppression hides data; the
rules are stated so a reader applies them, and so a table that violates them is
visibly wrong rather than quietly wrong.

### 4b. What this suite deliberately does not do

* **No CPU pinning, no governor forcing, no `nice`.** Developers do not build
  that way. The cost is variance, which R2 exposes rather than hides.
* **No cache dropping.** A cold page cache is not a situation anyone builds in,
  and it adds variance unrelated to the engine.
* **No engine-specific tuning.** Each engine gets the same standard, the same
  sources, the same compiler binary, the same optimisation level, and whatever
  its own documentation says is the normal way to build. Tuning one engine and
  not the others is how build-system benchmarks usually go wrong.
* **No confidence intervals.** Run counts are small by necessity; a computed
  interval would imply rigour that is not there. Medians with min/max and the
  raw samples are what the data supports.

### 4c. Practices this follows, and what it is not

Adopted, with the source of the practice:

| practice | from | here |
|---|---|---|
| full disclosure — host, tool versions, exact command, all flags | SPEC's run rules | §11 + every engine's version recorded by the engine itself |
| no benchmark-specific tuning | SPEC's run rules | §4b |
| warm-up run excluded from the timing | hyperfine, Google Benchmark | one untimed seed build per cell |
| report dispersion, not just a central value | hyperfine | `min_s`/`max_s`/`samples` + R2 |
| distinguish "cannot run" from "ran and failed" | — | `unavailable` vs `failed`, both requiring a reason |
| a versioned, machine-readable result format | — | `protocol_version` |

**What this is not.** It is not an audited or certified benchmark, there is no
reviewing body, and the numbers are single-host. Reproducing a published table
requires the same preset, the same engine versions and a comparable machine —
all of which the result file states, which is the point. Treat cross-machine
comparison of absolute seconds as invalid; compare **ratios within one table**.

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
* **Module support is a property of the engine *and* the compiler.** `supports()`
  therefore takes both, and a `false` becomes `unavailable` **with the
  measurement that produced it** — never a slow number. As measured here:

  | engine | modules with clang | modules with gcc |
  |---|---|---|
  | mcpp | yes | yes |
  | cmake ≥ 3.28 | yes | yes |
  | xmake 3.x | yes | yes |
  | bazel 9.2 + rules_cc 0.2.22 | **yes** | no — `aggregate-ddi failed … Invalid JSON string`, i.e. its ddi aggregator cannot parse GCC's P1689 output |
  | meson 1.10.2 | no — `fatal error: module 'fx.a' not found`; no attribute declares an interface unit | no |

  So a gcc run and a clang run legitimately have **different sets of populated
  cells**, and a table must say which compiler it used before its `unavailable`
  rows mean anything.
* **bazel module builds are forced to one object flavour.** `cc_binary` registers
  the ddi-aggregation action for both the PIC and the non-PIC object sets but
  names the output `<target>.CXXModules.json` for both, so analysis aborts before
  any compilation:

  ```
  Attempted action contains artifacts not in previous action: _objs/fx/unit_0.pic.ddi
  Previous action contains artifacts not in attempted action:  _objs/fx/unit_0.ddi
  Outputs: are equal
  ```

  The adapter passes `--force_pic` — to **every** variant, so bazel's own
  headers-vs-modules rows stay comparable, and PIC rather than
  `--features=-supports_pic` because it yields a PIE executable, which is what
  the other engines produce by default.

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

## 9. Building mcpp itself — `bench/projects/mcpp/`

Separate from the generated fixtures, `bench/projects/mcpp/` carries one build
description per foreign engine for **mcpp itself** — the control arm for "same
real project, different engine". Synthetic fixtures cannot reproduce the
dependency shape of a real 138-module codebase, and the difference is not small:
on the fixture mcpp's module cold build is 0.26x cmake, on mcpp's own source it
is **0.85x**. Both arms exist because either alone misleads.

```bash
bench --project . --buildfiles bench/projects/mcpp \
      --engines mcpp=<old>,mcpp=<new>,cmake,xmake \
      --compiler <path to g++> --baseline cmake \
      --hub src/platform/platform.cppm \
      --leaf src/version.cppm \
      --body src/build/stage.cppm
```

`--buildfiles` is what keeps these files **out of the repository root**. mcpp is
built by mcpp; a CMakeLists.txt and an xmake.lua at the root are files every
contributor has to learn to ignore, and one of them actively broke something:
`scripts/bootstrap-macos.sh` generates its own root `xmake.lua` when none is
present, and a bench-owned file at that path silently pre-empted it. cmake is
pointed at the directory with `-S`, xmake with `-P`; mcpp reads the project's
own manifest and ignores the flag. Copying the descriptions into the tree for
the duration of a run was the alternative, and it writes into the user's
repository, which this harness refuses to do.

| engine | builds mcpp? |
|---|---|
| mcpp | yes — it is mcpp's own manifest |
| cmake 4.0.2 | yes — needs `CMAKE_CXX_MODULE_STD 1` and the CMake-4.0 experimental UUID |
| xmake 3.0.7 | yes |
| meson 1.10.2 | no — no way to declare an interface unit, and no `import std;` |
| bazel 9.2.0 | not in the gcc table. `import std;` **is** buildable (libc++ ships the std module as ordinary source — see `bench/projects/mcpp/MODULE.bazel` for the working recipe), but bazel's modules need clang, so a bazel column belongs in a clang-baselined table or it breaks invariant I1 |

### The cmake description has two traps worth knowing

* **`FILE_SET CXX_MODULES` requires every file under a base directory.** The
  `mcpplibs.cmdline` dependency lives in the registry, outside the tree, so it
  needs its own file set with an explicit `BASE_DIRS`.
* **`add_compile_options()` does not reach the `std` module.** CMake generates
  that target itself, so directory-scope options miss it: the std module then
  compiles against the compiler's default libc headers while every mcpp unit
  compiles against `--sysroot`, and the build dies on a type that exists in both
  (`conflicting type for imported declaration 'char _IO_FILE::_unused2 [20]'`).
  The error names neither the flag nor the target that is wrong. Use
  `CMAKE_CXX_FLAGS`.

The xmake description pins the compiler by reading `[toolchain] default` out of `mcpp.toml`, because
the registry holds several GCCs and "newest directory wins" only *happens* to
agree with the pin. Verify before quoting anything from it:

```bash
xmake f -P bench/projects/mcpp -y -m release --toolchain=mcpp-gcc
xmake show -P bench/projects/mcpp -t mcpp | grep 'compiler (cxx)'   # must be mcpp's binary
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
