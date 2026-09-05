# `bench/results/` — one directory per measurement run

A run is a directory, not a filename prefix. The flat layout this replaces put
27 files from three unrelated runs side by side, sorted by tool name rather than
by run, so telling which JSON belonged to which table meant decoding timestamps.

Each directory holds its own `report.md` and the raw files that report was
written from, named by what varies **within** the run (host, compiler) rather
than repeating what the directory already says.

## Which of these is the standard data

**`standard-<date>-<os>-<arch>/` is the standard data set.** It is the only
directory produced by `bench/run-standard.sh`, the only one taken at 3 samples
per cell, and the only one the README tables quote. Everything else is history —
kept because a claim that cannot be checked against the run that produced it is
not a measurement, and deleted history cannot be checked at all.

**Every directory below the standard one was taken under conditions that no
longer hold.** Read them as a record of what was measured then, not as data
about mcpp now:

| taken with | what changed since |
|---|---|
| **cmake 4.0.2** | the pin is 4.4.2; its `import std` gate key is different, so those descriptions would not even configure today |
| **n=1** | the standard set is n=3; a single sample has no dispersion, which is why each of those tables carries a "do not compare the digits" caveat |
| **CI runners** | measured a shared 2-core machine — the same tree took 243s there and 79s on a developer box |
| **`bmi_schedule=on` before the §8b fix** | `touch-hub` and `edit-comment` in those columns were timing a build that had not finished; see ../README.md §8b |

| run | what it measures |
|---|---|
| [`five-way-20260812/`](five-way-20260812/) | six engines × three source forms × six scenarios, on a **generated fixture**. cmake is the baseline. Two compilers, one file each. |
| [`mcpp-self-20260813/`](mcpp-self-20260813/) | the same scenarios on the **real project** — mcpp building itself, 138 module interface units. cmake is the baseline. |
| [`pinned-workloads-20260813/`](pinned-workloads-20260813/) | **the first run in which everything that moves a number is pinned** — tools, compiler, measured sources, reference mcpp. mcpp building itself five ways, and xlings in two code styles. Earlier runs are not comparable to it. |
| [`hyperfine-20260812/`](hyperfine-20260812/) | the earlier one-off mcpp-vs-xmake runs, driven by hyperfine before the harness existed. Superseded by the two above; kept because `NOTES.md` records how those numbers were taken. |

**Read the reports, not the JSON.** The raw files are what makes a claim
checkable, but a number in them means nothing without the run's declared
asymmetries — those live in the report and in [`../README.md`](../README.md) §5.

**Generate the tables, do not type them.**

```bash
bench/tools/report.py <run>/*.json --baseline cmake
```

The tables in these reports used to be transcribed from harness output by hand,
and transcription is the one error this suite cannot catch: a mistyped headline
number is indistinguishable from a measured one, and no test will ever fail. The
generator also enforces two things a person forgets — a non-`ok` cell renders as
its status rather than as a blank or a zero, and a group whose cells used
different **perturbation forms** gets a footnote saying so.

**Before comparing anything across runs**, apply the validity rules in
[`../README.md`](../README.md) §4a: a cell within 2x of its own engine's `noop`
is measuring process startup, and absolute seconds do not carry between hosts —
only ratios within one table do.
