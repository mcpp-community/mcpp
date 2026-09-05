# standard-20260814-linux-x86_64

The Linux standard data set. **696 measured samples**, 3 per cell, produced by
`bash bench/run-standard.sh` on one machine in one sitting.

| | |
|---|---|
| host | Linux x86_64 · i9-13900K (24 physical / 32 logical, heterogeneous) |
| compilers | gcc 16.1.0, clang/libc++ 22.1.8 — both mcpp payloads, handed to **every** engine |
| foreign engines | cmake 4.4.2, xmake 3.1.0, bazel 9.2.0 |
| reference mcpp | 2026.8.11.3 (released) |
| **mcpp under test** | **built from `8b579fa`** — see the note below |
| started | 2026-08-15 (UTC date stamp `20260814`) |

## The commit is recorded HERE and not in the JSON

Every engine labels itself from `--version`, and mcpp's version is a **date**:
every commit on a branch reports `2026.8.13.1`. So these reports can say which
*release* they measured but not which *build* of it — and the numbers here move
with single commits.

`mbench --under-test` now records it, and `run-standard.sh` passes
`git rev-parse --short HEAD`. **These files predate that field.** The commit
above was recovered from the binary's mtime against `git log -- src/`, which is
weaker evidence than a recorded field; every later run states it in the JSON.

## Coverage

`-` means **not measured**. It never means "not applicable" and never means 0.

| toolchain | project | cells | outcome |
|---|---|---|---|
| gcc | `fixture` | 72 | 72 ok |
| clang | `fixture` | 90 | 90 ok |
| gcc | `mcpp-2026.8.11.3` | 25 | 25 ok |
| clang | `mcpp-2026.8.11.3` | 25 | 20 ok, 5 failed (xmake) |
| gcc | `xlings-2026.8.11.2` | 25 | 25 ok |
| gcc | `xlings-2026.8.13.1` | - | not measured — the run was stopped after five of seven cells |
| clang | `xlings-2026.8.11.2` | - | not measured — same |

`bash bench/run-standard.sh --resume` fills the last two in without repeating
any of the 696 samples above.

## The one failure

`clang / mcpp-2026.8.11.3 / xmake` — all five scenarios, `seed build exited
255`. Root cause reproduced by hand and filed upstream as
[#424](https://github.com/mcpp-community/mcpp/issues/424): xmake's default shape
on clang requires a **full** BMI, and publishing one to importers makes clang
22.1.8 fail on a downstream translation unit that uses a *narrow*
`std::format` string. Not waived with `allow_failed` — a reproduced failure is a
finding, and hiding it is what this suite exists to stop.

## The one declared outlier

`gcc / xlings-2026.8.11.2 / mcpp@2026.8.13.1+schedule=on / touch-hub` was
measured as `[1.77, 20.82, 1.79]` — a 1066% spread around a 1.79s median.

Re-measured immediately afterwards at 8 samples:
`[1.79, 1.79, 1.79, 1.79, 1.78, 1.79, 1.81, 1.79]` — 1% spread, no outlier. So
the 20.82s was machine noise, not an intermittent failure of the cascade
suppression. That probe is
[`probe-touch-hub-outlier-8-samples.json`](probe-touch-hub-outlier-8-samples.json).

**The published cell is left exactly as measured.** Splicing a second run's
samples into a first run's report is the one thing this suite must never do; the
probe is evidence *about* the number, not a replacement for it.

## Files

| file | |
|---|---|
| `<toolchain>-<project>.json` | the report — one per cell |
| `<toolchain>-<project>.log` | the harness's own output for that cell |
| `probe-touch-hub-outlier-8-samples.json` | the follow-up above; **not part of the standard set** |
