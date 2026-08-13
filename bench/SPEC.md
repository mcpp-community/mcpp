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
| **Build tool** | `mcpp` `cmake` `xmake` `meson` `bazel` | swept inside a job — `--engines` |
| **Project** | `fixture` `mcpp` `xlings` | one CI job each — `--project` |
| **Variant** | `headers` `modules` `modules-impl` | swept inside a job — `--variants` |
| **Scenario** | `cold` `noop` `touch-hub` `touch-leaf` `edit-body` `edit-comment` | swept inside a job — `--scenarios` |

**One CI job = one (OS, toolchain, project) cell.** The remaining three axes are
swept inside it, because they share a checkout, a toolchain install and a
generated fixture. Promoting them to jobs would multiply runner minutes without
adding a single measurement.

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
  all exist, so it is the only place the *variant* axis means anything.
* **`mcpp`** — 138 modules / 57k lines, one source dependency, build
  descriptions for all five engines under `projects/mcpp/`.
* **`xlings`** — 110 modules / 46k lines, **different authors**. This is the one
  that separates "a faster build engine" from "a faster benchmark target".

Real projects have exactly one form — their own — so their variant is `native`
and the harness refuses to generate over them.

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

---

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
`reason`. Two of those reasons currently say **KNOWN GAP** — `windows`+`gcc`,
and `xlings` on Windows. Those are meaningful cells that are simply not wired
up; they are written down so that "not measured" cannot quietly become "not
applicable".

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
