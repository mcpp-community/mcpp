# mcpp.buildopt

What mcpp does to a build beyond what the compiler and ninja do, in one package.

A build with none of it is the standard build: one edge per unit, every output the
compiler writes stands, and whatever follows an output is run. Each addition to that is
an entry of one register (`src/registry.cppm`). The pipeline asks at three seams —
source, graph, edge — and knows no entry. The specification is
[SPEC-019](../../docs/specs/build-optimizations.md); the user's chapter is
[docs/26](../../docs/26-build-optimizations.md).

## Entries

Every entry of low risk and measured benefit is on by default. The others are stated
by a parameter.

| Entry | Seam | Level | Default | Parameter | Effect |
|---|---|---|---|---|---|
| `keep-bmi` | edge | exact | on | — | A BMI compiled to the bytes it had is kept, and its importers are not compiled |
| `keep-object` | edge | exact | on | — | An object compiled to the bytes it had is kept, and nothing is archived or linked for it |
| `gcm-stamps` | edge | exact | on | — | The two wall-clock stamps of a GCC BMI are not a difference |
| `gcm-locations` | edge | conditional | on | `[profile.<name>] bmi_cutoff = "ignore-locations" \| "exact"`, `MCPP_BMI_CUTOFF` | A GCC BMI whose line maps alone moved is kept; its importers name the earlier lines |
| `scan-pass` | graph | exact | on | — | The scans of a graph run as a pass of their own before its compiles |
| `split-schedule` | graph | shape | off | `[build] bmi_schedule = "auto" \| "on" \| "off"`, `MCPP_BMI_SCHEDULE` | Importers start when a BMI exists, not when its compiler exits (two-phase with clang, detached code generation with gcc) |
| `private-fragment` | source | source | on | `[build] private_fragment = "unit" \| "conservative" \| "as-written"`, `MCPP_PRIVATE_FRAGMENT` | A private module fragment is built as a unit of its own, so an edit of it reaches no importer |
`MCPP_BUILDOPT=off` turns every entry off for one invocation: the standard build.
An entry with no parameter has nothing to choose — its products are the bytes the
standard build gives — and is turned off only by that statement.

## Levels

| Level | Meaning | Default |
|---|---|---|
| exact | The products are the bytes the standard build gives | on, no parameter |
| conditional | The program is the same; something beside it may differ (the lines an importer names) | on under the entry's guards, with a parameter |
| shape | The graph or its processes take another shape; the products are the same, the ways to fail are new | off until it has run on every host |
| source | A source is built as other units than it is written as | with a parameter; on where a source the entry is not sure of is built as written |

## Layout

| Module | File | Part |
|---|---|---|
| `mcpp.buildopt.registry` | `src/registry.cppm` | The register, and the statement that turns all of it off |
| `mcpp.buildopt.keep` | `src/keep/keep.cppm` | Edge seam: which previous BMI or object stays; the build directory's record |
| `mcpp.buildopt.keep.gcm` | `src/keep/gcm.cppm` | What is specific to GCC's BMI format |
| `mcpp.buildopt.schedule` | `src/schedule.cppm` | Graph seam: the shape of the module edges |
| `mcpp.buildopt.source` | `src/source.cppm` | Source seam: the units a source is built as |
| `mcpp.buildopt.advice` | `src/advice.cppm` | What a build says of the rebuild it made; changes nothing |

What holds and replaces files (the compile executor, `src/build/schedule`) and what
writes the graph (`src/build/ninja_backend.cppm`) are the pipeline's, and ask here.
A reader of another compiler's format is one more file under the part it belongs to.

## Recorded, not implemented

| Optimization | Expected benefit | Why not yet |
|---|---|---|
| A one-file module split into interface and implementation with no marker in the source | An edit of a function body reaches no importer, without the developer writing `module :private;` | Needs semantic analysis (which definitions are inline, templates, constexpr; which bodies read what is local to the interface). A semantic provider can supply it through the source seam |
| Clang: a BMI kept when the interface an importer can observe is unchanged | With clang, any edit of a body or a comment stops at the module | Measured 2026-11-26 on clang 20.1.7 to 23.1.6: the hash of a non-inline function (the one field that moves with its body) sits at an operand that varies with the declaration (26, 27 and 29 for member functions of one version) and with the version; a third of the edits change the width of the 32-bit hash in the stream, which moves the bit offsets and three offset tables after it. Without a model of the declaration records a reader cannot tell that field from an interface change |
| The tests a change affects, by symbol | `mcpp test --since` runs fewer tests | Needs references across imports; dynamic dispatch can hide a test |
| Two versions of one library in one program, renamed apart | A dependency chain that names two versions builds | Possible for module-only packages; headers, C libraries and macros are not renamed. No measurement of how often it occurs |
