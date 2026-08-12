# `bench/results/` — one directory per measurement run

A run is a directory, not a filename prefix. The flat layout this replaces put
27 files from three unrelated runs side by side, sorted by tool name rather than
by run, so telling which JSON belonged to which table meant decoding timestamps.

Each directory holds its own `report.md` and the raw files that report was
written from, named by what varies **within** the run (host, compiler) rather
than repeating what the directory already says.

| run | what it measures |
|---|---|
| [`five-way-20260812/`](five-way-20260812/) | six engines × three source forms × six scenarios, on a **generated fixture**. cmake is the baseline. Two compilers, one file each. |
| [`mcpp-self-20260813/`](mcpp-self-20260813/) | the same scenarios on the **real project** — mcpp building itself, 138 module interface units. cmake is the baseline. |
| [`hyperfine-20260812/`](hyperfine-20260812/) | the earlier one-off mcpp-vs-xmake runs, driven by hyperfine before the harness existed. Superseded by the two above; kept because `NOTES.md` records how those numbers were taken. |

**Read the reports, not the JSON.** The raw files are what makes a claim
checkable, but a number in them means nothing without the run's declared
asymmetries — those live in the report and in [`../README.md`](../README.md) §5.

**Before comparing anything across runs**, apply the validity rules in
[`../README.md`](../README.md) §4a: a cell within 2x of its own engine's `noop`
is measuring process startup, and absolute seconds do not carry between hosts —
only ratios within one table do.
