# Building mcpp with four engines — 2026-08-13

The **real project**, not a fixture: 138 module interface units, 57k lines, every
one of them `import std;`. cmake is the baseline; every cell shows the median
wall time and its ratio to cmake in the same row.

| | |
|---|---|
| host | Linux x86_64 · 13th Gen Intel Core i9-13900K · 32 logical / 24 physical · 64 GiB |
| workload | mcpp itself, measured in place (`--project .`) |
| compiler | `gcc@16.1.0`, the hermetic mcpp payload, pinned into every engine |
| engines | mcpp 2026.8.11.3 and 2026.8.12.1, cmake 4.0.2 + ninja, xmake v3.0.7+HEAD |
| build files | `bench/projects/mcpp/` (`--buildfiles`), so nothing foreign sits at the repo root |
| perturbed | hub `src/platform/platform.cppm` (46 importers) · leaf `src/pm/publisher.cppm` (0) · body `src/build/stage.cppm` |
| runs | 2 per cell |
| raw | [`linux-x86_64-gcc.json`](linux-x86_64-gcc.json) |

| scenario | mcpp@2026.8.11.3 | mcpp@2026.8.12.1 | cmake | xmake |
|---|---|---|---|---|
| `cold` | 80.49s · 0.85x | 82.87s · 0.88x | **94.53s** · 1.00x | 94.63s · 1.00x |
| `noop` | 0.28s · 0.83x | 0.20s · 0.58x | **0.34s** · 1.00x | 0.38s · 1.10x |
| `touch-leaf` | 17.39s · 0.96x | 2.14s · 0.12x | **18.06s** · 1.00x | 18.47s · 1.02x |
| `edit-body` | 18.30s · 0.93x | 18.29s · 0.93x | **19.64s** · 1.00x | 19.97s · 1.02x |
| `edit-comment` | 76.50s · 0.90x | 0.46s · 0.01x | **85.03s** · 1.00x | 84.69s · 1.00x |
| `touch-hub` | 76.50s · 0.91x | 0.44s · 0.01x | **84.53s** · 1.00x | 83.65s · 0.99x |

---

## What this says

### 1. On cold builds, all four engines are within 15% of each other

80.5–94.6s. The synthetic fixture put mcpp at **0.26x** cmake; here it is
**0.85x**. Anyone quoting the fixture ratio as mcpp's cold-build advantage is
quoting an artefact of a workload whose units cost 0.09s each.

The reason nobody wins is structural: mcpp's cold build is **100% critical
path** (79.73s of a 79.79s makespan, average parallelism 3.94x of 32 hardware
threads). Every engine walks the same 26-deep chain of module interfaces, so
scheduling cannot help and neither can more cores. See
`.agents/docs/2026-08-12-cold-build-optimization-plan.md` for the measured
headroom (BMI is complete at ~22% of each compile; the other 78% is code
generation nobody downstream needs).

### 2. On the scenarios that dominate a working day, the gap is ~190x

Touching the most-imported unit — `mcpp.platform`, 46 importers — costs cmake
**84.53s** and xmake **83.65s**: they rebuild the world because the BMI's mtime
moved. mcpp 2026.8.12.1 costs **0.44s**, because it compares the BMI the
compiler just produced against the previous one and puts the old file back when
they are equivalent.

That is **192x** against the baseline and **174x** against mcpp's own previous
release, which had the same mechanism and never once fired: GCC stamps a wall
clock into every BMI, so the byte compare it used could never report "unchanged".

`edit-comment` — a real content change that leaves the interface alone — is the
same story at 185x.

### 3. `edit-body` shows no gain, and that is the point

18.29s for both mcpp releases, 19.64s for cmake. Editing a function body in
`src/build/stage.cppm` genuinely changes that unit's BMI, so the cascade is
**correct** and every engine pays it. A mechanism that made this row fast too
would be skipping rebuilds it must not skip.

The row is in the table for exactly that reason: it is the control that
distinguishes "avoids unnecessary work" from "avoids work".

### 4. `touch-leaf` costs 17–18s for everyone but the new mcpp

A unit nobody imports still takes 17.4s to rebuild under cmake, xmake and the
previous mcpp — because it is one of the fat ones, and its own compile is that
expensive. 2.14s for mcpp 2026.8.12.1 is the same BMI-equivalence check firing
one level down.

---

## Caveats

* **Single host, two runs per cell.** Under §4a's dispersion rule the cold rows
  (80–95s, spreads under 2%) are solid; `noop` at 0.20–0.38s is within 2x of the
  engines' own floor and should be read as "all four are instant", not a ranking.
* **cmake and xmake compile `mcpplibs.cmdline` from source; mcpp stages it from
  its global cache.** Three units, ~1s. It is declared here rather than hidden,
  and it does not move any conclusion above.
* **No bazel column.** bazel builds C++20 modules only with clang — its ddi
  aggregator cannot parse GCC's P1689 output — so including it here would break
  the "same compiler binary" invariant. `import std;` itself is *not* the
  blocker: libc++ ships the std module as ordinary source and bazel builds it
  fine (recipe in `bench/projects/mcpp/MODULE.bazel`). A bazel column needs a
  clang-baselined table.
* **No meson column.** meson 1.10.2 has no way to declare a module interface
  unit, and no `import std;` story.
