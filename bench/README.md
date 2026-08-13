# `bench/` — build-engine benchmark suite

**English** · [简体中文](README.zh-CN.md)

A cross-platform harness for measuring **build engines** against each other on
the **same C++ sources**, and for measuring what C++20 named modules actually
cost compared to headers.

Written in C++23 and built by mcpp, so it runs identically on Linux, macOS and
Windows — a shell-based harness cannot, and this suite replaced one that could
only run on Linux.

```bash
# generated fixtures, across engines and source forms
bench --engines mcpp,cmake,xmake,bazel \
      --variants headers,modules,modules-impl \
      --scenarios cold,noop,touch-hub,edit-body \
      --compiler /path/to/g++ --jobs 32 --out report.json

# a REAL project — one of the PINNED workloads under bench/projects/,
# comparing two mcpp binaries
bench --project bench/projects/mcpp/mcpp-2026.8.11.3 \
      --buildfiles bench/projects/mcpp \
      --engines mcpp=./target/*/*/bin/mcpp,mcpp --compiler payload:gcc \
      --scenarios noop,touch-hub --hub src/platform/platform.cppm
```

Each `mcpp=<path>` engine labels itself from the version that binary reports
(`mcpp@2026.8.13.1`), so two releases never collapse into one row. That is how
"did this release get faster?" is answered — by running both, not by emulating
one of them in the harness.

### The three documents, and which one to read

| file | answers |
|---|---|
| **this file** | *how* a timing is taken, and what is deliberately not controlled |
| [`SPEC.md`](SPEC.md) | *what* is measured: the six axes, why cmake is the baseline, what a cell being undefined means |
| [`matrix.json`](matrix.json) | *which* cells CI runs — the single source, read by `.github/workflows/bench.yml` |

The cell list appears in exactly one of those. A matrix written down twice is a
matrix that disagrees with itself, and the disagreement is silent: both copies
keep looking right. `tests/e2e/233_bench_matrix.sh` is what keeps it that way.

---

## 0. What is pinned, and why every one of these is pinned

A benchmark number is only worth the list of things that were held still while
it was taken. Every row below was loose at some point in this suite's short
life, and every one of them produced a table that was measuring something other
than what it said.

| what | pinned to | declared in |
|---|---|---|
| cmake | **4.0.2** | `matrix.json` → `tools` |
| xmake | **3.1.0** | `matrix.json` → `tools` |
| bazel | **9.2.0** | `matrix.json` → `tools` |
| gcc | **16.1.0** | `bench/src/toolchain.cppm` |
| clang / libc++ | **22.1.8** (Windows: 20.1.7) | `bench/src/toolchain.cppm` |
| reference mcpp | **2026.8.11.3** | `matrix.json` → `reference_mcpp` |
| mcpp (the workload) | **2026.8.11.3** — `a749e9f` | submodule `projects/mcpp/mcpp-2026.8.11.3` |
| xlings (combined style) | **2026.8.11.2** — `b1563fe` | submodule `projects/xlings/xlings-2026.8.11.2` |
| xlings (split style) | **2026.8.13.1** — `f072075` | submodule `projects/xlings/xlings-2026.8.13.1` |
| mcpp under test | the checkout | built by CI, resolved by `newest_artifact.sh` |

**Everything is installed by xlings**, at those exact versions, on every runner.
`xlings install cmake@4.0.2 xmake@3.1.0 bazel@9.2.0 mcpp@2026.8.11.3` is
literally what CI runs, and the job prints the resolved version of each one and
warns loudly if it is not the pinned one.

Four things this bought, each of which had already gone wrong:

* **cmake 3.31.6** is what the GitHub runner images ship. It does not have the
  CMake 4.0 experimental key for `import std`, so *every module cell failed to
  configure*. With 4.0.2 they pass.
* **`command -v g++`** on those images is gcc 13.3.0. cmake cannot configure
  C++23 modules with it and xmake crashes it with an internal compiler error —
  while mcpp quietly used its own registry's gcc 16.1 regardless. The table read
  `48 failed / 6 ok` and was still called a comparison of build engines. The
  suite now hands **every** engine the driver out of mcpp's own payload
  (`--compiler payload:gcc`), which is its fairness rule finally enforced rather
  than merely written down.
* **The measured workloads moved.** xlings was `git clone --depth 1` of its
  default branch at run time, so the target changed with every upstream push —
  `--hub src/xlings.cppm` had been naming a file that no longer existed for
  months, every xlings cell reported `skipped`, and every xlings job reported
  success. mcpp's own sources had the same defect in a form that is harder to
  see: `--project $GITHUB_WORKSPACE` made the checkout the workload, so every
  commit on a branch silently changed the thing being measured. **The engine
  under test is the binary and is supposed to move; the workload is not.**
  All three are git submodules now, and the guard checks that each `hub` and
  `body` exists in the pinned tree.
* **Only one mcpp was measured.** A report that says how fast this branch is,
  without saying whether it got faster, is not what a benchmark on a pull
  request is for.

> **Not held still, and deliberately so:** the runner hardware. See §4a.

> **Two ways the measured tree does get written to.** Neither affects a timing,
> but both leave a dirty submodule:
>
> 1. **The engine's own bookkeeping.** `mcpp build` writes `mcpp.lock`, cmake and
>    xmake write into `build/`. That is the engine doing its job — a real user's
>    build does it too — so it is not something the harness should prevent.
> 2. **A hard-killed run.**
>    The editing scenarios save a file's exact bytes and restore them however
>    the function exits — including on a failed build — but that is a
>    destructor, and a destructor does not run under `SIGKILL`. Interrupt a
>    `--project` run hard enough and the perturbation is still there. They are
>    named `bench_nonce_*`, so they are easy to recognise in a diff.
>
> `git submodule foreach 'git checkout -- .'` undoes both.

> **Not exercised by these numbers:** mcpp's split build schedule
> (`[build] bmi_schedule = "on"`) is opt-in until it has been verified on every
> platform, so both mcpp binaries run with it off. Its effect is measured
> separately in `.agents/docs/2026-08-13-build-optimization-status.md`.

### The headline numbers, and where they come from

> ⚠️ **`bmi_schedule` has a known correctness bug — do not quote these numbers.**
> On the generated fixture's `modules` variant, four scenarios fail with
> `failed to read compiled module: No such file or directory` in an importer.
> It reproduces at `-j1`, so it is not a race between compilers: phase 1 parks
> the previous BMI in `.bak` *before* spawning the compiler, and the file is
> measurably absent for ~208 ms of every rebuild. Every `bmi_schedule` figure
> below was taken with that defect present. See
> `.agents/docs/2026-08-13-build-optimization-status.md` §8.

**Read the real-project table first.** A synthetic fixture is for isolating one
variable; it is not evidence about anyone's build. Where the two disagree, the
real project is right and the fixture is telling you about its own shape.

#### mcpp itself — the pinned workload, 137 modules, 57k lines, gcc 16.1.0

`bench/projects/mcpp/mcpp-2026.8.11.3` (`a749e9f`), measured in place with
`--buildfiles projects/mcpp/`, i9-13900K, **n=1** (see the caveat below).
Ratios against cmake.

| scenario | `mcpp@2026.8.11.3` | `mcpp@2026.8.13.1` | `+bmi_schedule=on` | `cmake` | `xmake` |
|---|---|---|---|---|---|
| `cold`         | 79.46s · 0.86x | 79.54s · 0.86x    | **35.43s · 0.38x** | **92.33s** · 1.00x | 90.30s · 0.98x |
| `noop`         | 0.34s · 1.21x  | 0.16s · 0.57x     | 0.16s · 0.57x      | **0.28s** · 1.00x  | 0.38s · 1.36x |
| `touch-hub`    | 76.53s · 0.92x | **0.40s · 0.005x** | **0.22s · 0.003x** | **83.39s** · 1.00x | 82.08s · 0.98x |
| `edit-body`    | 77.33s · 0.90x | 76.24s · 0.89x    | **30.17s · 0.35x** | **85.64s** · 1.00x | 84.61s · 0.99x |
| `edit-comment` | 75.69s · 0.91x | **0.38s · 0.005x** | **0.18s · 0.002x** | **82.96s** · 1.00x | 82.73s · 1.00x |

Four things this says, and the fixture can say none of them:

1. **On a cold build nobody wins, and that is the correct answer.** Every engine
   is within 15% of the others, because mcpp's cold build is **100% critical
   path** — 79.7s of a 79.8s makespan, average parallelism 3.94 of 32 hardware
   threads. All of them walk the same 26-deep chain of module interfaces, and
   scheduling cannot shorten a chain. The generated fixture puts mcpp at `0.26x`
   here; that is an artefact of a workload whose units cost 0.09s each, and
   quoting it as a cold-build advantage would be dishonest.
2. **The cold-build lever is the opt-in schedule, not the release.** 79.46s →
   79.54s between the two releases is no change at all; `bmi_schedule = "on"`
   takes it to 35.43s. Everything else in this table is release-over-release;
   that column is a *setting*.
3. **The daily loop is where the engines differ**, by ~190x on this project:
   touching a hub interface costs cmake and xmake a full 83-second rebuild
   because they decide by timestamp, and 0.40s for an engine that compares the
   BMI it just produced against the previous one.
4. **`edit-body` is the control.** mcpp is deliberately *not* fast there (0.89x):
   the interface genuinely changed, so the cascade is owed. An engine that were
   fast on that row would have skipped work it owed.

> **The xmake column is from a SEPARATE run.** Its numbers in the original
> five-arm run were invalid — xmake normalises `--buildir` to a path relative to
> `-P` and then resolves it against the process cwd, so `clean()` had been
> removing a directory it never wrote to and `cold` came back at **0.60s** with
> status `ok`. Fixed (the engine now runs from `-P`) and re-measured on the same
> machine; `cold` went 0.58s → 90.95s in the isolated check and 90.30s here.
> Recorded rather than quietly re-run, because the two halves of this table were
> not taken in the same minute.

> **n=1, so read the ratios and not the digits.** §4a R2 asks for dispersion and
> a single sample has none. Two rows also sit near their own engine's resolution
> floor: mcpp's `touch-hub` and `edit-comment` are 2.5x and 2.4x its own `noop`,
> just above R1's 2x line, so *"about two orders of magnitude"* is supported and
> *"0.40 versus 0.38"* is not.

> **`edit-comment` here is the `end-of-file` form.** mcpp's hub has no function
> body, so the comment is appended rather than inserted, and no line numbers
> move. On a hub that does have bodies the same scenario legitimately cascades —
> see the xlings table below and SPEC.md §4. The cell's `note` records which
> form ran.

#### xlings — the same question on someone else's codebase, in two code styles

110 modules, 46k lines, different authors, never tuned for this. The two pins
are the same project either side of one refactor. Ratios against the released
mcpp.

#### The three engines, on the combined tree

First measurement in which all three arms produce a running binary — the cmake
and xmake columns below were `failed` cells until the arms were finished, and the
table that stood here was mcpp-against-mcpp for that reason.

| scenario | **mcpp** `bmi_schedule=on` | mcpp default | cmake | xmake |
|---|---|---|---|---|
| `cold` | **37.56s** · 3.18x | 92.49s · 1.29x | 119.46s · 1.00x | 105.02s · 1.14x |
| `noop` | **0.72s** · 0.50x | 0.74s · 0.49x | 0.36s · 1.00x | 0.40s · 0.90x |
| `touch-hub` | **1.04s** · 93.93x | 1.79s · 54.96x | 98.16s · 1.00x | 98.16s · 1.00x |
| `edit-body` | **29.65s** · 3.32x | 88.38s · 1.11x | 98.43s · 1.00x | 98.00s · 1.00x |
| `edit-comment` | **30.44s** · 3.22x | 93.81s · 1.04x | 97.98s · 1.00x | 97.97s · 1.00x |

**Both mcpp columns are here because one of them was misleading on its own.**
The default column is what a user gets today; `bmi_schedule=on` is the opt-in
split schedule, and leaving it out understated mcpp badly — `edit-body` reads
1.11x in the default column and 3.32x with the schedule on.

* **`edit-body` and `edit-comment` are not "no advantage".** The cascade really
  is owed in both (the perturbed function body lives in an interface unit, so
  the BMI genuinely changes). The default column shows mcpp doing that owed work
  at cmake's pace; the schedule column shows it doing the SAME work 3.3x faster,
  by publishing each BMI as soon as it exists instead of after code generation.
* **`noop` is the one mcpp loses outright**, in both columns: 0.72–0.74s against
  cmake's 0.36s. That is per-invocation overhead, and it is the number a user
  feels on every edit-build cycle.
* **The `bmi_schedule` correctness bug (§8b) does NOT reproduce here.** All ten
  cells are `ok`. It reproduces on the generated fixture, whose tight
  unit_0→unit_1 chain hits the window; three real trees (mcpp's own and both
  xlings styles) do not. That is why the key is still opt-in — a defect that
  only one workload can show is still a defect.

<sub>xlings `2026.8.11.2`, gcc 16.1.0 payload, Linux x86_64 · i9-13900K · n=1 ·
`--baseline cmake`. Raw report: `bench/results/xlings-3way-20260814/`.</sub>

Three things in that table are worth reading carefully, because two of them are
mcpp LOSING:

* **`noop` is 0.49x — mcpp is the slowest of the three at doing nothing.** 0.74s
  against cmake's 0.36s. It is a fixed cost on every invocation, and it is the
  one number here that a user feels on every keystroke-to-build cycle.
* **`edit-comment` is 1.04x, not the 200x the mcpp workload shows.** The comment
  lands INSIDE an inline function body that xlings keeps in its interface unit,
  so the BMI genuinely changes and the cascade is owed. The cell's note records
  which form ran; see §3, and do not read this as the optimisation failing.
* **`touch-hub` is the real result: 54.96x.** Content unchanged, so mcpp compares
  the BMI it just produced against the previous one and skips 45 importers.
  cmake and xmake decide by timestamp and rebuild all of them — to within 0.00s
  of each other, which is what two timestamp-driven engines should look like.


#### The same three engines on the SPLIT tree

Same project, implementations moved out of the interface units. This is the axis
the two pins exist for, and it changes the answer more than the engine does.

| scenario | **mcpp** | cmake | xmake |
|---|---|---|---|
| `cold` | **27.59s** · 1.82x | 50.13s · 1.00x | 41.90s · 1.20x |
| `noop` | **0.79s** · 0.44x | 0.34s · 1.00x | 0.50s · 0.68x |
| `touch-hub` | **1.32s** · 20.08x | 26.60s · 1.00x | 31.68s · 0.84x |
| `edit-body` | **1.79s** · 0.75x | 1.35s · 1.00x | 1.49s · 0.91x |
| `edit-comment` | **24.17s** · 1.09x | 26.35s · 1.00x | 31.28s · 0.84x |

<sub>xlings `2026.8.13.1`, `modules-impl`, same host and payload as above. Raw
report: `bench/results/xlings-3way-20260814/xlings-split-3way.json`.</sub>

* **The refactor beats every engine choice on this workload.** `cold` falls from
  92.49s to 27.59s for mcpp — 3.35x — and cmake's own cold falls 119.46s → 50.13s
  (2.38x). Moving implementations out of interface units buys more than switching
  build tool does.
* **`edit-body` is 0.75x — mcpp is SLOWER than cmake here**, 1.79s against 1.35s.
  With the body in a `.cpp`, one object recompiles and nothing cascades, so the
  scenario measures per-invocation overhead rather than graph reasoning — the
  same fixed cost `noop` shows. On this axis mcpp has no advantage to offer and
  the number says so.
* **`touch-hub` still pays: 20.08x.** Smaller than the combined tree's 54.96x
  because there is simply less downstream work left to skip.

#### The two code styles, mcpp against mcpp

Older run, kept because it is the only side-by-side of the two pinned
styles. Both columns are mcpp, so the foreign arms being unfinished at the
time does not affect it.

| scenario | combined `2026.8.11.2` old → new | split `2026.8.13.1` old → new | what the split buys |
|---|---|---|---|
| `cold`         | 97.01s → 92.48s | 30.33s → 29.78s ⁽ⁿ⁼³⁾ | **3.11x** |
| `noop`         | 1.55s → 0.72s   | 1.62s → 0.76s   | — |
| `touch-hub`    | 89.39s → **1.76s** (50.6x) | 24.87s → **1.30s** (19.1x) | 1.35x |
| `edit-body`    | 89.46s → 88.33s | 2.73s → **1.77s** | **49.96x** |
| `edit-comment` | 95.40s → 95.02s | 25.09s → 25.29s | 3.76x |

Those `new` columns are the DEFAULT build. With the opt-in BMI schedule on the
same binary:

| tree | scenario | default | `+bmi_schedule=on` | |
|---|---|---|---|---|
| combined | `cold`      | 92.95s | **43.26s** | **2.15x** |
| combined | `edit-body` | 91.66s | **30.19s** | **3.04x** |
| split    | `cold`      | 27.62s | 29.72s     | 0.93x |
| split    | `edit-body` | 1.79s  | 1.79s      | 1.00x |

**The two levers overlap, and the code style is the bigger one.** The schedule
buys time by letting importers start as soon as a BMI exists — so it only helps
when there is a cascade to overlap. Splitting the implementations out removes
the cascade instead: 92.95s → 27.62s cold and 91.66s → 1.79s on an edit, after
which the schedule has nothing left to win (and costs a little on `cold`).

If you are choosing one, choose the code style. The schedule is what helps a
codebase that has not made that change.

* **Splitting implementations out of the interface units is worth 2.6x on a cold
  build and ~50x on `edit-body`.** That is the largest single effect in this
  whole suite, and it is a *code style*, not an engine feature.
* **`touch-hub` reproduces the engine result on a codebase nobody tuned for it**
  — 50.6x, against 190x on mcpp's own tree. Different magnitude, same mechanism.
* ⚠️ **The `cold` row was nearly published as a 23% REGRESSION.** At n=1 the
  split tree read `29.13s → 35.88s`, i.e. the new mcpp slower. Re-measured at
  n=3 it is `30.33s → 29.78s` — marginally *faster*. The single pair had simply
  caught the new arm near the old arm's maximum: the old arm's spread is
  **19.1%** (29.92–35.72), a hair under the 20% that §4a R2 calls noisy, while
  the new arm's is 4.7%. This is R2 doing exactly what it is for, and it is why
  every other row here says n=1 rather than pretending otherwise.
* **`edit-comment` does not improve at all here (1.00x), and that is correct.**
  xlings' hub has 56 function bodies, so inserting a comment moves every
  subsequent line; GCC records inline-body source locations in the BMI, the BMI
  genuinely changes, and the cascade is owed. mcpp's own hub has none, which is
  the entire reason that row reads 199x there and 1.00x here. **A project
  measuring itself cannot discover this.**

#### The generated fixture — 40 units, fan-in 3


Full data: [`results/five-way-20260812/`](results/five-way-20260812/). Useful
because it is the only place `headers` / `modules` / `modules-impl` can be
compared as a controlled variable, and because it covers clang and bazel too.

`modules`, **gcc 16.1.0**:

| scenario | mcpp@2026.8.11.3 | mcpp@2026.8.12.1 | cmake | xmake |
|---|---|---|---|---|
| `cold`        | 3.61s · 0.28x | 3.53s · 0.27x | **13.05s** · 1.00x | 11.46s · 0.88x |
| `noop`        | 0.15s · 0.46x | 0.14s · 0.42x | **0.34s** · 1.00x  | 0.32s · 0.94x  |
| `touch-leaf`  | 0.39s · 0.39x | 0.30s · 0.31x | **0.99s** · 1.00x  | 1.16s · 1.17x  |
| `touch-hub`   | 3.61s · 0.35x | **0.29s · 0.03x** | **10.32s** · 1.00x | 11.13s · 1.08x |
| `edit-comment`| 3.67s · 0.36x | **0.30s · 0.03x** | **10.31s** · 1.00x | 10.55s · 1.02x |
| `edit-body`   | 3.65s · 0.35x | **0.29s · 0.03x** | **10.29s** · 1.00x | 11.15s · 1.08x |

`modules`, **clang 22.1.8**:

| scenario | mcpp@2026.8.11.3 | mcpp@2026.8.12.1 | cmake | xmake | bazel |
|---|---|---|---|---|---|
| `cold`        | 2.65s · 0.66x | 2.50s · 0.62x | **4.00s** · 1.00x | 13.19s · 3.30x | 3.19s · 0.80x |
| `noop`        | 0.18s · 0.57x | 0.18s · 0.54x | **0.32s** · 1.00x | 0.32s · 0.99x  | 0.20s · 0.63x |
| `touch-hub`   | 0.35s · 0.13x | 0.28s · 0.10x | **2.67s** · 1.00x | 12.76s · 4.79x | 0.23s · 0.08x |
| `edit-body`   | 0.52s · 0.20x | 0.46s · 0.17x | **2.62s** · 1.00x | 12.68s · 4.84x | 2.84s · 1.08x |

**What the old-vs-new column is actually showing.** Under gcc, 3.65s → 0.29s on
`edit-body` is not a scheduling change. Both releases have the same mechanism —
compare the BMI the compiler just produced against the previous one, and when
they are equivalent put the old file back so ninja's `restat` sees no change —
but 2026.8.11.3 compared **bytes**, and GCC writes `buildtime:`/`localtime:`
stamps into every BMI. No two BMIs were ever byte-equal, so the suppression had
never once fired since it was written in May.

Under clang the same rows barely move, because clang's cold build is already
3.3× cheaper than gcc's and there is far less cascade to avoid. That is the
whole argument for the toolchain being an axis: **the answer is not the same
multiple on both**, so a suite that pinned one compiler would publish one of
these two numbers as if it were the answer.

> ⚠️ Those numbers were taken with **cmake 4.0.2 / xmake 3.0.7**, before the
> pins in the table above. They are quoted here because they are a real,
> reproducible, in-repo result file; CI now runs the pinned versions and the
> tables are refreshed from its artifacts. Do not mix rows from the two.

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
| I1 | Identical compiler **binary** across engines | `--compiler <path>` is threaded into cmake (`-DCMAKE_CXX_COMPILER`), xmake (`CXX`), bazel (`CC` + `--action_env`). mcpp uses its hermetic payload — a **declared asymmetry**, see §5. |
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

* **`cold` includes configure.** cmake keeps its configure output inside the
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

* ~~**mcpp uses its own hermetic toolchain.**~~ **CLOSED, and it was not an
  asymmetry — it was a hole.** mcpp resolves gcc/llvm from its registry and
  ignores the `--compiler` every other engine is handed, which for the generated
  fixture is harmless (the harness writes that manifest) and for a real project
  is not: the pinned workloads say `gcc@16.1.0`, so on a clang cell cmake and
  xmake ran clang while mcpp quietly ran gcc — a compiler comparison wearing an
  engine-comparison label. `--compiler payload:gcc|clang` now resolves the driver
  out of mcpp's own registry for *every* engine, and the mcpp engine translates
  the same request into `MCPP_TOOLCHAIN` from the same version constants. Stated
  here because it stood as a "declared asymmetry" for a while, and a thing you
  can fix should not stay on this list.
* **The `+schedule=on` arm is the same binary, not a different engine.** mcpp's
  BMI schedule is a key in the MEASURED PROJECT's manifest and the workloads are
  pinned (one belongs to someone else), so the harness reaches it through
  `MCPP_BMI_SCHEDULE` and labels the arm `mcpp@<ver>+schedule=on`. It is an
  option under test, and it is on the same row set as the default so the two are
  read together rather than across runs.
* **No fixture says `import std;`.** Engines differ wildly in how — and whether —
  they can build the std module (CMake needs a per-version experimental UUID,
  meson has no story). That difference would dominate every measurement. The
  fixtures reach the standard library through the global module fragment, which
  every engine handles identically. **This suite measures module machinery, not
  std-module support.**
* **The three arms do not obtain their dependencies the same way, and their
  `cold` columns are therefore not the same quantity.** xlings links ftxui,
  libarchive, lua and mbedtls, which mcpp's registry ships as SOURCE. The cmake
  and bazel arms compile those sources themselves — from each package's own
  `.xpkg.lua`, so the file list matches mcpp's exactly — which means their
  `cold` includes ~470 dependency translation units. The xmake arm declares them
  through xrepo instead, the way xlings' own `xmake.lua` does, so it links
  libraries xrepo built earlier and its `cold` does not include them.
  `xmake clean` does not evict the xrepo package cache, so this is stable across
  runs rather than a first-run artefact — but it is a real difference in
  workload, not a difference in engine speed. Compare `cold` across engines on
  the FIXTURE, which has no third-party dependencies at all; on xlings, compare
  the incremental scenarios, where no arm rebuilds a dependency.
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

## 9. Real projects — `bench/projects/`

| target | what it is for |
|---|---|
| [`mcpp/`](projects/mcpp/) | mcpp building itself, with cmake/xmake/bazel descriptions beside it |
| [`common/`](projects/common/) | the per-engine payload logic both projects share — one branch per compiler family |
| [`xlings/`](projects/xlings/) | an **independent** codebase (110 modules / 46k lines, different authors) — the control that separates "a faster build engine" from "a faster benchmark target" |

⚠️ **An engine change that only helps the project it was developed on is not an
engine change.** The split module schedule was developed against mcpp (2.30x)
and reproduces on xlings at **3.38x**; that second number is the one that makes
it a general result. Conversely, restructuring a target's modules speeds up that
target and nobody else's — see `.agents/docs/2026-08-13-build-optimization-status.md`
§L3/L4 for why those stay out of the engine's own PR.

### 9a. Building mcpp itself — `bench/projects/mcpp/`

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

The suite's own tests live in [`tests/`](tests/) rather than in mcpp's e2e
directory: `bench/` is meant to be extractable into its own project, and mixing
its tests into the host repository would put that one rename away from breaking.
mcpp's `tests/e2e/230_bench_harness.sh` is a five-line delegator, kept so the
harness does not silently drop out of every mcpp PR — `bench.yml` is
workflow_dispatch-only and nothing else would run it.

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
