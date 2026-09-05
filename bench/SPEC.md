# Benchmark specification

What this suite measures, what a cell is, and which cells the standard data set covers.

The **cell list itself is not here** — it is [`matrix.json`](matrix.json), which
`bench/run-standard.sh` reads to plan a run. A matrix written down
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
| **OS** | `linux` `macos` `windows` | the runner selects the cells for the machine it is on |
| **Toolchain** | `gcc` `clang` `msvc` | one cell each — `--compiler` |
| **Build tool** | `mcpp` `cmake` `xmake` `bazel` | swept inside a job — `--engines` |
| **Engine options** | `mcpp[schedule=on]` | an ARM of an engine, not a fifth engine — `--engines` |
| **Project** | `fixture` `mcpp-2026.8.11.3` `xlings-2026.8.11.2` `xlings-2026.8.13.1` | one cell each — `--project` |
| **Variant** | `headers` `modules` `modules-impl` | swept inside a job — `--variants` |
| **Scenario** | `cold` `noop` `touch-hub` `touch-leaf` `edit-body` `edit-comment` | swept inside a job — `--scenarios` |

**One cell = one (OS, toolchain, project).** The remaining axes are swept
inside it, because they share a checkout, a toolchain install and a generated
fixture. Promoting them to jobs would multiply runner minutes without adding a
single measurement.

**An engine may appear more than once in the same cell**, and a row is named by
what the binary reports rather than by the token that asked for it:

| spec | row |
|---|---|
| `mcpp=<the build under test>` | `mcpp@2026.8.13.1` |
| `mcpp=<the released reference>` | `mcpp@2026.8.11.3` |
| `mcpp[schedule=on]=<the build under test>` | `mcpp@2026.8.13.1+schedule=on` |

That is how "did this release get faster?" and "what does the opt-in key buy?"
are answered — by running each and letting them label themselves, never by one
arm standing in for another. `[...]` is an option list understood by the
registry, which rejects a spec whose option it does not know rather than
silently ignoring it; `run-standard.sh` gives the reference arm to the bare
token only, because the released binary predates the fix in README §8b and
measuring its scheduler would read as a regression in the feature.

### Everything the number depends on is pinned

Not a tidiness preference. Each of these was unpinned once, and each produced a
table that was measuring something other than what it said:

| pinned | where | what it cost while it was loose |
|---|---|---|
| cmake, xmake, bazel | `matrix.json.tools` | runner images ship cmake 3.31.6, which lacks the CMake 4.0 `import std` key, so **every module cell failed to configure** |
| the compiler | `bench/src/toolchain.cppm` | engines got `command -v g++` = gcc 13.3.0 while mcpp used the registry's gcc 16.1 — cmake could not configure, xmake crashed gcc outright |
| the workloads | git submodules under `bench/projects/` | xlings was cloned from its default branch at run time (`--hub src/xlings.cppm` named a file that had stopped existing); **mcpp's own sources were the checkout**, so every commit on a branch changed the thing being measured |
| the reference mcpp | `matrix.json.reference_mcpp` | a report said how fast this branch is, never whether it got faster |

> The reference pin is **not** required to equal the `.xlings.json` workspace
> pin. A guard once required that, on the theory that the reference arm is
> whatever CI bootstraps; neither half holds (the standard set runs on a
> developer box, and the bootstrap pin is a self-hosting floor that may lag a
> release), and bumping the pin after a release turned every e2e shard red on
> `main`. `run-standard.sh` resolves the arm by exact version and requires the
> binary to report that version itself, so a mismatch drops the column with a
> note instead of measuring the wrong release.

`--compiler payload:gcc` / `payload:clang` is the spelling that delivers the
second row: it resolves to the driver **inside mcpp's own registry**, so every
engine is handed the same binary. That is the suite's fairness rule
(`resolve_cxx`) actually enforced rather than merely written down.

**mcpp needed a second half of that fix.** It resolves its own toolchain from
the *measured project's* manifest and ignores `--compiler` entirely. For the
generated fixture that is harmless, because the harness writes that manifest —
but the real workloads are pinned submodules whose `[toolchain]` says
`gcc@16.1.0`, so on a clang cell cmake and xmake ran clang while mcpp quietly
ran gcc. The mcpp engine now translates the requested compiler into
`MCPP_TOOLCHAIN` (the side channel `--toolchain` uses), from the same version
constants `payload:` resolves against, so the driver the other engines get and
the toolchain mcpp is told to use cannot name different versions.

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

The same rule applies one level up, to cells the standard set does not cover at all:
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
| `edit-body` | a real semantic edit inside a function body | the everyday loop — and whether a cascade is owed depends on **where the body lives and whether the edit moves lines**, not on what the body now does. See below. |
| `touch-leaf` | mtime bump on a unit nobody imports | recompile 1 + link |

#### `edit-body` perturbs a DIFFERENT FILE in each variant, and the two ask
#### opposite questions

| variant | file perturbed | what a correct engine does |
|---|---|---|
| `headers` | `unit_0.cpp` | recompile 1 + link — never a cascade |
| `modules` | `unit_0.cppm` (interface) | **may cascade**, see below |
| `modules-impl` | `unit_0_impl.cpp` (implementation unit) | recompile 1 + link — **never** a cascade |

An implementation unit produces no BMI, so nothing can depend on it; the absence
of a cascade there is structural. An interface unit is the opposite: whether the
edit cascades depends on the compiler and on the edit.

**The perturbation inserts a line.** Under GCC that shifts the recorded source
location of every declaration after the insertion point, which changes the BMI —
so the cascade follows from a changed BMI rather than from the edited body. The
same edit expressed as a same-line substitution does **not** cascade on GCC.
Under clang the interface cascades either way, because clang serialises
definitions into the BMI regardless of where in the file they appear.

**The generated fixture does not reproduce this**: its perturbed function is
the LAST declaration in `unit_0.cppm`, so nothing shifts and the BMI is unchanged
(measured: 0.94s, against 80.87s for the same scenario on the mcpp tree). Any
conclusion about a real project drawn from the fixture's `edit-body` is invalid
— which is what `--project` mode exists for. Full measurements in
[`.agents/docs/2026-08-15-module-edit-granularity.md`](../.agents/docs/2026-08-15-module-edit-granularity.md).

`edit-comment` exists **separately from `edit-body`** on purpose: without the
split, an engine that skips comment-only rebuilds can be advertised as "12x
faster on edits", which is a claim about comments.

#### `edit-comment` has two forms, and the report says which one ran

The comment goes **inside the first function body**. A unit with no function
body — a `modules-impl` interface, or a hub that only declares — has nowhere to
put it, so it is appended at end of file instead. Those are different
perturbations:

| form | what moves | BMI | expected result |
|---|---|---|---|
| `in-body` | every subsequent line in the file | **changes** — GCC records inline-body source locations | a cascade is CORRECT |
| `end-of-file` | nothing | unchanged | an engine comparing BMIs skips the cascade |

Measured the same day, same engine, same compiler: `edit-comment` on mcpp's hub
(66 lines, no bodies → `end-of-file`) was **0.38s**, and on xlings' hub (566
lines, 56 bodies → `in-body`) was **95.02s**. Side by side and without the form,
that reads as "the optimisation works on one project and not the other" — which
is not what happened. The generated fixture splits the same way, `modules` going
in-body and `modules-impl` end-of-file.

So the form is written into the cell's `note`
(`… · perturbation: in-body`). Same rule as a non-`ok` status carrying its
reason: **a number whose meaning depends on an invisible choice is not a
measurement.**

`edit-body` is the control that keeps the suite honest in the other direction —
where a cascade IS owed, no engine should be fast, and one that is has skipped
work it owed.

#### But a body edit does not always owe a cascade, and that is the point

Measured directly, GCC 16.1, comparing the BMI before and after:

| what is edited | BMI | cascade |
|---|---|---|
| a body in a `.cppm`, edit **moves lines** (inserts or deletes one) | **differs** | **owed** |
| a body in a `.cppm`, edited **in place** (same line count) | **byte-identical** | not owed |
| a body in a separate `.cpp` implementation unit | **no BMI exists** | not owed |

GCC 16.1 does not serialise non-template function bodies, so changing what a
body *does* is invisible to importers. What it does serialise is the source
position of each declaration — so inserting a line moves every declaration
after it and the BMI changes for that reason alone.

**An earlier version of this section said the deciding factor was whether the
body belonged to an exported class.** That was reasoning, and the measurement
refuted it: editing `Version::str()` — a member of an exported class — in place
rebuilt its object and left the BMI byte-identical, so no importer was touched.
The deciding factor is line movement, not class membership.

Two consequences:

* "editing one function rebuilt forty modules" is not inherent to named modules.
 It follows from the edit moving lines in an interface unit.
* the third row is the sturdiest, because it holds for **every** compiler and
  for every edit: a `.cpp` implementation unit produces no BMI, so nothing
  downstream can depend on its contents. Clang, whose BMI carries more than
 GCC's, cascades on an in-place body edit in a `.cppm` but not on a `.cpp`.

**This is what the two xlings pins measure.** Moving the implementations out of
the interface units takes `edit-body` from 88.33s to **1.77s** on the same
project — ~50x, the largest single effect anywhere in this suite, and a code
style rather than an engine feature.

#### KNOWN GAP: there is no scenario for an in-place body edit

The three rows in the table above are not equally covered. `edit-body` inserts a
statement, so only the **first** row is ever measured; the second — a semantic
edit that keeps the line count — has no scenario at all.

That is the everyday case, and it is the only one that would show cascade
suppression on a *real code change* rather than on a timestamp (`touch-hub`) or
a comment (`edit-comment`). Its absence makes the published tables read as
though the effect applies only when the code does not change, which understates
it.

Closing it is a `replace_in_first_body` beside `insert_into_first_body` (an
equal-length substitution, e.g. one integer literal for another of the same
width) plus a scenario token — and a re-run of the standard set, which is why it
is recorded here rather than half-added with no data behind it.

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

## 5. What the standard data set covers

`bench/run-standard.sh` plans one run per entry in `matrix.json.cells` whose
`os` matches the machine it is on, and runs **every engine that cell lists** at
**3 samples** each.

**`allow_failed` is NOT consulted by the runner.** Those waivers were recorded
against failures on a shared CI runner, and at least two of them describe arms
that configure and generate perfectly well on a developer machine. Filtering by
them would carry a runner's limitation into local data and publish a smaller
comparison than the machine can make. They stay in the file as the record of
what broke where; the runner ignores them, and a failure here is a failure here.

**There is no CI job for this suite.** There was, and 12 of its 32 foreign-engine
arms were waived — xmake had more arms waived than measured — while the job
reported success. `tests/e2e/230_bench_harness.sh` still runs on every PR and
checks that the suite builds and emits a valid report; the measuring is manual,
and belongs before a release and after any change claiming a performance effect.

