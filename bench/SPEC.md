# Benchmark specification

What this suite measures, what a cell is, and which cells CI runs.

The **cell list itself is not here** — it is [`matrix.json`](matrix.json), which
`.github/workflows/bench.yml` reads to plan its jobs. A matrix written down
twice is a matrix that disagrees with itself, and the disagreement is silent:
both copies keep looking right.

`README.md` is the measurement contract (how a timing is taken, what is
deliberately not controlled). This file is the *shape* of the measurement.

---

## 1. The axes

A measurement is identified by six coordinates. Five of them are already the
result schema's `CellKey` (`bench/src/protocol.cppm`); the sixth is the host,
which the report records in its run facts.

| axis | values | where it is chosen |
|---|---|---|
| **OS** | `linux` `macos` `windows` | one CI job each |
| **Toolchain** | `gcc` `clang` `msvc` | one CI job each — `--compiler` |
| **Build tool** | `mcpp` `cmake` `xmake` `bazel` | swept inside a job — `--engines` |
| **Project** | `fixture` `mcpp-2026.8.11.3` `xlings-2026.8.11.2` `xlings-2026.8.13.1` | one CI job each — `--project` |
| **Variant** | `headers` `modules` `modules-impl` | swept inside a job — `--variants` |
| **Scenario** | `cold` `noop` `touch-hub` `touch-leaf` `edit-body` `edit-comment` | swept inside a job — `--scenarios` |

**One CI job = one (OS, toolchain, project) cell.** The remaining three axes are
swept inside it, because they share a checkout, a toolchain install and a
generated fixture. Promoting them to jobs would multiply runner minutes without
adding a single measurement.

### Everything the number depends on is pinned

Not a tidiness preference. Each of these was unpinned once, and each produced a
table that was measuring something other than what it said:

| pinned | where | what it cost while it was loose |
|---|---|---|
| cmake, xmake, bazel | `matrix.json.tools` | runner images ship cmake 3.31.6, which lacks the CMake 4.0 `import std` key, so **every module cell failed to configure** |
| the compiler | `bench/src/toolchain.cppm` | engines got `command -v g++` = gcc 13.3.0 while mcpp used the registry's gcc 16.1 — cmake could not configure, xmake crashed gcc outright |
| the workloads | git submodules under `bench/projects/` | xlings was cloned from its default branch at run time (`--hub src/xlings.cppm` named a file that had stopped existing); **mcpp's own sources were the checkout**, so every commit on a branch changed the thing being measured |
| the reference mcpp | `matrix.json.reference_mcpp` | a report said how fast this branch is, never whether it got faster |

`--compiler payload:gcc` / `payload:clang` is the spelling that delivers the
third row: it resolves to the driver **inside mcpp's own registry**, so every
engine including mcpp is handed the same binary. That is the suite's fairness
rule (`resolve_cxx`) actually enforced rather than merely written down.

### Two mcpp binaries, always

`mcpp` in a cell's engine list expands to **two** engines: the mcpp built from
the checkout and `reference_mcpp` installed by xlings. Each labels itself from
the version it reports, so the rows never collapse — and the harness warns if
two binaries claim the same version, because then they silently would.

> **Not covered by that column:** the split build schedule (`[build] bmi_schedule =
> "on"`) is opt-in until it has been verified on every platform, so both
> binaries run with it OFF. These numbers therefore do not include it; see
> `.agents/docs/2026-08-13-build-optimization-status.md` for its separately
> measured effect.

### Why the toolchain is an axis and not a detail

Because the answer changes with it, and not by a constant factor. On mcpp's own
sources the compiler alone is worth **2.5x** (gcc 81.8s → clang 32.6s), and the
engine-level optimisation on top of that is worth a *different* multiple on each
(gcc 2.30x, clang 1.78x). A suite that pinned one compiler would report one of
those two numbers as if it were the answer.

`--compiler` is passed to **every** engine that accepts one. An engine left on
its host default turns the comparison into compiler-vs-compiler while still
being labelled engine-vs-engine — see `resolve_cxx` in
`bench/src/engines/engine.cppm`, where that rule is enforced.

### Why the project is an axis

`fixture` is generated and calibrated, so it isolates one variable at a time.
Real projects are the control that stops an engine change from being an artefact
of one graph shape:

* **`fixture`** — synthetic, parameterised (`--preset`, `--units`, `--fanin`,
  `--weight`). The only project where `headers` / `modules` / `modules-impl`
  are all generated, so it is where the *variant* axis is a controlled variable.
* **`mcpp-2026.8.11.3`** (`a749e9f`) — 137 modules / 57k lines, one source
  dependency, build descriptions for every engine under `projects/mcpp/`.
  Pinned like everything else: the engine under test is the binary, and a
  workload that moves with the branch makes two runs incomparable.
* **`xlings-2026.8.11.2`** and **`xlings-2026.8.13.1`** — 110 modules / 46k
  lines, **different authors**. This is what separates "a faster build engine"
  from "a faster benchmark target".

### The two xlings pins are a code-style comparison

They are the same project either side of one refactor:

| project | shape | variant |
|---|---|---|
| `xlings-2026.8.11.2` (`b1563fe`) | 110 `.cppm` + **2** `.cpp` — each interface unit carries its own implementation | `modules` |
| `xlings-2026.8.13.1` (`f072075`) | 110 `.cppm` + **92** `.cpp` — implementations split out | `modules-impl` |

Same module graph, same line count, opposite answers to "where does the code
live" — which is exactly the `modules` vs `modules-impl` axis the generated
fixture has, except on a real codebase written by people who were not thinking
about this benchmark. `--body` differs accordingly: editing an implementation
means the `.cpp` in the split style and the `.cppm` in the combined one.

**One description serves both.** `projects/xlings/{CMakeLists.txt,xmake.lua}`
glob `src/**/*.{cppm,cpp}` — the same rule mcpp itself infers from — so neither
style needs its own file, an environment switch, or a branch. Globbing only
`src/main.cpp`, which is what they used to do, compiles the split style's
interfaces, links nothing, and still reports a time.

A real project has exactly one form — its own — so a cell states which of the
two names it, and the harness never generates over the tree.

---

## 2. cmake is the baseline

Every ratio in every report is against cmake, and the harness defaults
`--baseline` to it rather than leaving it unset.

Not an arbitrary pick:

* it is the reference implementation of C++ module builds — P1689 scanning and
  ninja `dyndep` are its design, and every other engine here implements *its*
  protocol;
* it is present on every machine this suite runs on, so the ratio exists in
  every cell;
* a reader already has a feel for it. An absolute second count means nothing
  without knowing the runner; **"1.8x cmake" survives being read on a different
  machine**, which is the only way these numbers travel.

A run whose engine set omits cmake prints `(no successful 'cmake' cell here;
ratios omitted)` rather than a table of bare seconds — the one form of this data
that cannot be compared to anything.

### A cell may override it, and the xlings cells do

`matrix.json` lets a cell name its own `baseline`. The xlings cells normalise
against the released mcpp instead, because their cmake and xmake arms compile
every translation unit and then **stop at the link**: xlings pulls ftxui,
libarchive, lua and mbedtls in as *source* packages that mcpp compiles, so the
foreign arms want symbols nobody built (`undefined reference to mbedtls_*`).

Both arms are kept anyway — a documented wall is data, and the day someone adds
`add_subdirectory` for those four the cell turns green by itself — but they are
listed in that cell's `allow_failed` so a known gap does not fail the run. The
guard requires a waiver to name an engine the cell actually has *and* to carry a
`KNOWN GAP` note, because a waived failure that says nothing is a hidden one.

Overriding the baseline is what turns that cell from "a table of bare seconds"
into the comparison it can actually make: **mcpp against mcpp**, which is the
question a control target exists to answer.

---

### meson is not an engine here

meson 1.10.2 has no way to declare a translation unit to be a module
**interface**. Listing `.cppm` files as ordinary sources compiles them as plain
TUs and the first importer fails with `fatal error: module 'x' not found`, and
there is no `import std;` equivalent either. So every module cell was an
`unavailable` row with the same reason — one honest row and five empty ones per
report, which is noise rather than a comparison. It was removed: engine,
descriptions and fixture emitter.

The day meson grows the feature, the diff is adding
`bench/src/engines/meson.cppm` back and one line in `registry.cppm`.

## 3. A cell may be undefined, and it must say why

Three outcomes are distinguishable in the result schema, and collapsing them is
the failure this suite is built to avoid:

| status | meaning |
|---|---|
| `ok` | measured; `samples` present |
| `failed` | the engine ran and did not produce the artifact — a real finding |
| `unavailable` | the engine is not installed here |
| `skipped` | this engine cannot express this cell — `note` says what is missing |

`note` is **required** whenever the status is not `ok`. "No number" and "zero
seconds" must never render the same way, and neither must "not installed" and
"cannot do this".

The same rule applies one level up, to cells that CI does not run at all:
[`matrix.json`](matrix.json) carries an `excluded` list where every entry has a
`reason`, and the ones that say **KNOWN GAP** are meaningful cells that are
simply not wired up rather than cells that make no sense — written down so that
"not measured" cannot quietly become "not applicable". An exclusion may also
name an `engine`, which scopes a caveat to one COLUMN instead of removing the
job: the cell still runs, and its note says what to distrust.

---

## 4. The scenarios, and what each one is for

| scenario | perturbation | the question |
|---|---|---|
| `cold` | no build dir | full graph construction + every compile |
| `noop` | nothing | how cheap is "already up to date" |
| `touch-hub` | mtime bump on a widely-imported unit, **content unchanged** | can the engine prove the interface did not change? |
| `edit-comment` | a comment inserted into that same unit | the bytes *did* change but the interface did not — only an engine that compares the produced BMI avoids the cascade |
| `edit-body` | a real semantic edit inside a function body | the everyday loop. For an inline body in an interface unit the BMI legitimately changes and a cascade is **correct** |
| `touch-leaf` | mtime bump on a unit nobody imports | recompile 1 + link |

`edit-comment` exists **separately from `edit-body`** on purpose: without the
split, an engine that skips comment-only rebuilds can be advertised as "12x
faster on edits", which is a claim about comments.

`edit-body` is the control that keeps the suite honest in the other direction —
there, no engine should be fast, and one that is has skipped work it owed.

Real projects run five of the six: `touch-leaf` needs a unit nobody imports
*and* a stable name for it, which a generated fixture has by construction and a
real tree does not.

---

### Shared build descriptions

`projects/common/` holds the parts every arm needs: `cmake/hermetic_payload.cmake`
and `xmake/payload.lua`. Both answer one question — "make this engine drive the
same process tree mcpp does" — and both are **one branch per compiler family**,
because that is what makes the toolchain a real axis rather than a label.

They exist because the two projects had two copies of it, and the copies were
already diverging. Two copies of a toolchain definition is the worst place for a
copy: they drift by one flag and the benchmark reports the difference between the
two *descriptions* as an engine result.

## 5. What CI runs

`.github/workflows/bench.yml` plans one job per entry in `matrix.json.cells`,
`fail-fast: false` — one platform missing an engine must not cancel another
platform's data.

Triggers: any change under `bench/**` except `*.md` and `results/**`, plus
`workflow_dispatch`. Docs and past results are excluded because a README edit
cannot move a number, and running a long matrix to prove that is how a check
becomes one people ignore. `results/**` is excluded for a sharper reason too:
this workflow's own artifacts land there, so including it would let a results
commit trigger the run that produces the next results.

**No thresholds, no pass/fail on timings.** Cloud runners are shared and the CPU
model changes underneath you; a threshold there converts normal variance into
red crosses that get muted. Reports upload as artifacts and comparing them is a
human act.

`tests/e2e/233_bench_matrix.sh` checks this file against the harness: every
value named in `axes` must be one the harness actually accepts, every cell must
draw from those axes, and the workflow must not carry a second hard-coded copy
of the list.
