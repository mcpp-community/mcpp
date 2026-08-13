# Pinned workloads — 2026-08-13

The first run of this suite in which **everything that moves a number is pinned**:
the tool versions, the compiler, the measured sources, and the reference mcpp.
Previous runs are not comparable to this one, and the reason is not subtle — see
[`../../SPEC.md`](../../SPEC.md) §1 and the audit note at the bottom.

| | |
|---|---|
| host | Linux x86_64 · 13th Gen Intel Core i9-13900K · 32 logical / 24 physical (heterogeneous) |
| compiler | `gcc 16.1.0`, mcpp's own payload, handed to **every** engine (`--compiler payload:gcc`) |
| tools | cmake 4.0.2 + ninja, xmake 3.0.7 — *local versions; CI pins 4.4.2 / 3.1.0 / 9.2.0* |
| workloads | `mcpp-2026.8.11.3` (`a749e9f`, 137 modules) · `xlings-2026.8.11.2` (`b1563fe`) · `xlings-2026.8.13.1` (`f072075`) — all git submodules |
| repetitions | **n=1**, except `xlings-split-cold-n3` |

Regenerate any table below from the raw files rather than transcribing it:

```bash
bench/tools/report.py bench/results/pinned-workloads-20260813/mcpp-linux-gcc-5way.json
```

---

## 1. mcpp building mcpp — five arms

Ratios against cmake. `+bmi_schedule=on` is the **same binary** with the opt-in
BMI schedule enabled via `MCPP_BMI_SCHEDULE`.

| scenario | `mcpp@2026.8.11.3` | `mcpp@2026.8.13.1` | `+bmi_schedule=on` | `cmake` | `xmake` |
|---|---|---|---|---|---|
| `cold`         | 79.46s · 0.86x | 79.54s · 0.86x     | **35.43s · 0.38x** | **92.33s** · 1.00x | 90.30s · 0.98x |
| `noop`         | 0.34s · 1.21x  | 0.16s · 0.57x      | 0.16s · 0.57x      | **0.28s** · 1.00x  | 0.38s · 1.36x |
| `touch-hub`    | 76.53s · 0.92x | **0.40s · 0.005x** | **0.22s · 0.003x** | **83.39s** · 1.00x | 82.07s · 0.98x |
| `edit-body`    | 77.33s · 0.90x | 76.24s · 0.89x     | **30.17s · 0.35x** | **85.64s** · 1.00x | 84.61s · 0.99x |
| `edit-comment` | 75.69s · 0.91x | **0.38s · 0.005x** | **0.18s · 0.002x** | **82.96s** · 1.00x | 82.73s · 1.00x |

* **Nobody wins `cold`, and that is correct.** All within 15%. mcpp's cold build
  is 100% critical path — 79.7s of a 79.8s makespan, average parallelism 3.94 of
  32 threads — so every engine walks the same 26-deep interface chain.
* **The cold lever is the setting, not the release.** 79.46 → 79.54 between
  releases is nothing; `bmi_schedule = "on"` takes it to 35.43s (2.24x).
* **`touch-hub` / `edit-comment` are ~190x**, and they sit 2.5x / 2.4x above
  mcpp's own `noop` — just past R1's floor, so read them as two orders of
  magnitude, not as three digits.
* **`edit-comment` here is the `end-of-file` form**: mcpp's hub has no function
  body, so nothing shifts. See §3.

⚠️ **The `xmake` column is from a separate run** (`…-xmake-refixed.json`). In the
five-arm file its `cold` reads **0.60s** — invalid. xmake normalises `--buildir`
to a path relative to `-P` and resolves it against the process cwd, so `clean()`
had been removing a directory nothing ever wrote to. Fixed, re-measured, and the
harness now refuses a `cold` that is under 2x its own `noop`.

## 2. xlings — two code styles, mcpp against mcpp

The same project either side of one refactor. cmake and xmake are absent because
their arms stop at the link here (SPEC.md §2), so the baseline is the released
mcpp.

| scenario | combined `2026.8.11.2` old → new | split `2026.8.13.1` old → new | what the split buys |
|---|---|---|---|
| `cold`         | 97.01s → 92.48s | 30.33s → 29.78s ⁽ⁿ⁼³⁾ | **3.11x** |
| `noop`         | 1.55s → 0.72s   | 1.62s → 0.76s   | — |
| `touch-hub`    | 89.39s → **1.76s** (50.6x) | 24.87s → **1.30s** (19.1x) | 1.35x |
| `edit-body`    | 89.46s → 88.33s | 2.73s → **1.77s** | **49.96x** |
| `edit-comment` | 95.40s → 95.02s | 25.09s → 25.29s | 3.76x |

* Splitting implementations out of the interface units is worth **3.1x cold** and
  **~50x on an edit**. A code style, not an engine feature — and the largest
  single effect anywhere in this suite.

### 2b. …and the opt-in schedule on top of it

| tree | scenario | default | `+bmi_schedule=on` | |
|---|---|---|---|---|
| combined | `cold`      | 92.95s | **43.26s** | **2.15x** |
| combined | `edit-body` | 91.66s | **30.19s** | **3.04x** |
| split    | `cold`      | 27.62s | 29.72s     | 0.93x |
| split    | `edit-body` | 1.79s  | 1.79s      | 1.00x |

**The two levers overlap, and the code style is the bigger one.** The schedule
lets importers start as soon as a BMI exists, so it only helps where there is a
cascade to overlap. Splitting the implementations removes the cascade instead,
after which the schedule has nothing left to win — and costs a little on `cold`.

Raw: `xlings-combined-schedule-linux-gcc.json`, `xlings-split-schedule-linux-gcc.json`.
* `touch-hub` reproduces the engine result on a codebase nobody tuned for it.

⚠️ **The `cold` row was nearly published as a 23% regression.** At n=1 it read
`29.13s → 35.88s`. At n=3 it is `30.33s → 29.78s`, marginally faster: the single
pair had caught the new arm near the old arm's max. The old arm's spread is
**19.1%** — a hair under the 20% that §4a R2 calls noisy.

## 3. The finding that only a second project could produce

`edit-comment` is **199x on mcpp's own tree and 1.00x on xlings**. Not an
optimisation that works sometimes:

| hub | lines | `) {` anchors | perturbation form | result |
|---|---|---|---|---|
| mcpp `src/platform/platform.cppm` | 66 | **0** | `end-of-file` — nothing shifts | BMI unchanged, cascade skipped |
| xlings `src/platform.cppm` | 566 | **56** | `in-body` — every later line shifts | BMI changes, **cascade is owed** |

GCC records inline-body source locations in the BMI. mcpp measuring itself could
never have seen this, because its hub happens to have no function bodies. The
form is now recorded in every cell's `note`.

---

## What changed about the suite itself before these numbers could be trusted

The previous matrix reported success while measuring almost nothing: one job was
**6 ok / 48 failed / 18 unavailable**, and every xlings job had zero
measurements. Six independent causes, every one of them a failure that looked
like a success. `.agents/docs/2026-08-13-build-optimization-status.md` §7 has the
full list; the assertions added as a result are in
[`../../SPEC.md`](../../SPEC.md) §3 and `tests/e2e/233_bench_matrix.sh`.
