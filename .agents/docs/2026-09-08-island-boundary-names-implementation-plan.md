---
subject: heterogeneous
status: active
---

# Implementation plan: the island boundary's names

Executes [2026-09-08-island-boundary-names.md](2026-09-08-island-boundary-names.md)
across three repositories. One pull request per repository, ordered by what each
one's CI needs to be green.

## Global constraints

* `mcpp:plugins` 0.5.0. The floor stays **mcpp 2026.9.8.1** -- nothing here uses
  an engine API newer than 0.4.0 already required, so `MCPP_VERSION` in the
  plugins CI does not move and neither does any index floor.
* The generated header stays flat, `extern "C"`, free of namespaces, and is what
  a device compiler reads. Only the `.cppm` gains namespaces.
* No compatibility flag. The four call sites are updated in the same batch.
* Prose is declarative and free of decoration; no emoji in documentation, in
  code comments, or in commit messages.
* Every generated name passes through one sanitiser, shared by both lanes.

## The order, and why it is not free

```
  T1..T5  mcpp-plugins PR ──> merge ──> tag v0.5.0 ──> GitHub release
                                                        │
                                                        ├─> gtc: CN asset
                                                        │
                                                   T6  mcpp-index PR (0.5.0 + latest)
                                                        │
                                                   T7..T10  mcpp PR (docs + examples)
                                                        │
                                                   T11 sandbox verification
```

`examples/09-heterogeneous/boundary` is built by mcpp's own CI
(`.github/tools/build_examples.sh`) and resolves `mcpp:plugins` through the
index, so the mcpp pull request cannot be green until 0.5.0 is published and
indexed. The plugins fixtures use `path = "../.."` and are therefore independent.

---

## T1 -- one sanitiser, shared by both lanes

**Files:** `src/plugins.cppm` (add `mcpp::plugins::names`),
`rules/spirv.cppm` (call it instead of defining it).

`identifier`, `split_module_name`, `common_base_dir` and `namespace_of` become
`mcpp::plugins::names::*`. `surface` and the island generator both call them, so
a directory named `2d` or `default` gets one answer rather than two that agree
today by inspection.

Exported inline functions in this package must not range-for over a
`std::string`: GCC 16 then instantiates `std::string::iterator` in the BMI and
every consumer's build program fails to compile in `<bits/stl_iterator.h>`.
Index instead. `tools/island.cppm` records the measurement.

*Criterion:* `rules/spirv.cppm` contains no definition of `common_base_dir` or
`namespace_of`, and `tests/spirv-module-consumer` still emits
`namespace default_ {`.

## T2 -- roots, layout root, and namespaced emission

**Files:** `tools/island.cppm`.

```cpp
struct options {
    std::string module_name;                  // the module root
    std::string out_dir;
    std::string produced_by;
    bool        emit_module   = true;
    std::string marker        = "MCPP_EXPORT_C";
    std::vector<std::string> roots;           // directories, or single files
    std::string layout_root;                  // default: roots.front()
    std::vector<std::string> extensions;      // default: device table + C/C++
    std::string strip_prefix;                 // empty: no short name
};

struct entry {
    std::string decl;                         // verbatim, as scanned
    std::string name;                         // the identifier before `(`
    std::vector<std::string> name_space;      // segments below the layout root
    std::string origin;                       // the file it was first seen in
};

std::optional<std::vector<entry>> scan(const options&);
std::optional<emitted>            emit(std::span<const entry>, const options&);
entry declared(std::string decl, std::vector<std::string> ns = {});  // rung L2
```

`scan` walks each root in declaration order, sorted by path, reading files whose
extension is in the set. For each marked declaration it records the entry, its
name and the namespace segments of its directory relative to its own root. An
entry's namespace is the one it has in the layout root; an entry the layout root
does not declare keeps the namespace of the first root that does.

`emit` writes one flat header and one module. The module groups entries by
namespace path and opens each block once.

*Criteria:* the four fixtures below.

## T3 -- the two checks

**Files:** `tools/island.cppm`.

* A name declared twice within one root is refused, naming both files and saying
  that two implementations of one entry point belong in two roots.
* A name declared in several roots whose declarations differ is refused, naming
  both files and both declarations. This is the check 0.4.0 performs; what
  changes is that it no longer doubles as the collision check.
* `scan` that finds no marked entry point in any root is an error naming the
  roots. An empty module is a misspelled path or a marker that never arrived.

## T4 -- the short name

**Files:** `tools/island.cppm`.

`strip_prefix` non-empty emits `inline constexpr auto <short> = <name>;` beside
`using ::<name>;`. The short name passes through `names::identifier`. Two entries
stripping to one short name are refused naming both. An entry not carrying the
prefix gets no short name.

Measured 2026-09-08: `app::kernels::image::blur == &app::kernels::image::opkit_blur`
under clang++ (DPC++ 7.1.0) `-std=c++23`, and the same declarations compile under
GCC 13.

## T5 -- fixtures and CI

**Files:** `tests/island-interface/**`, `.github/workflows/ci.yml`,
`README.md`, `mcpp.toml` (0.5.0), `src/plugins.cppm` (the version constant).

The fixture gains a subdirectory in the layout root and a flat fallback file, so
the namespace, the flat-fallback allowance and the layout root's authority are
all exercised by the shape of the tree rather than by an assertion about it:

```
src/kernels/saxpy.c            island_saxpy_device   -> ::kernels
src/kernels/image/scale.c      island_scale_device   -> ::kernels::image
src/cpu/ops.c                  both, flat
```

CI steps, each with a denominator:

1. the module carries `export namespace island_interface::kernels {` and
   `export namespace island_interface::kernels::image {`;
2. the header carries neither `namespace` nor any of the short names;
3. the flat fallback is accepted and `image` survives -- moving `src/cpu/ops.c`
   to `src/cpu/deep/ops.c` leaves the generated `.cppm` byte-identical;
4. two files in ONE root declaring one name are refused naming both;
5. two entries stripping to one short name are refused naming both;
6. roots that hold no marker are refused naming the roots;
7. the short name and the long name are one entity, and the artifact holds one
   symbol for the pair;
8. the 0.4.0 disagreement check still fires, unchanged;
9. the CPU leg (`--no-accel`) reaches the same qualified names.

## T6 -- mcpp-index

**Files:** `pkgs/m/mcpp.plugins.lua`.

A `0.5.0` entry in each of the three platform tables with the release tarball's
sha256, and `["latest"] = { ref = "0.5.0" }` in each. The header comment gains
the paragraph describing what 0.5.0 changes. Existing consumers pin exact
versions, so moving `latest` breaks none of them.

*Criterion:* the descriptor parses under `mcpp xpkg parse`, holds exactly one
`["latest"]` per platform table, and the new version resolves in a sandbox
**through an exact pin**.

The obvious criterion -- resolve it through `latest` -- cannot be written.
Measured in a sandbox: `version = "latest"` in a manifest is taken as a literal
wire address and fails with `install path missing after fetch`; omitting
`version` is refused by the manifest parser; and `mcpp add mcpp:plugins` answers
`package version required (M2 supports exact-version only)`. So **no mcpp
consumer path reads the `latest` alias today** -- it is consumed by xlings'
own resolution. Moving it is correct and costs nothing, and the criterion has
to be the descriptor's content plus an exact-pin resolution.

## T7 -- examples

**Files:** `examples/09-heterogeneous/boundary/**`,
`examples/09-heterogeneous/cuda/app/**`, `examples/09-heterogeneous/sycl/app/**`.

Each pins `plugins = { version = "0.5.2", ... }`, passes `roots` and a
`layout_root`, and reaches the boundary through the qualified name. `boundary`
additionally shows `strip_prefix`, because it is the example whose whole subject
is the generated interface.

## T8 -- documentation

**Files:** `docs/42-heterogeneous-builds.md`, `docs/zh/42-...`,
`docs/31-authoring-a-rule-package.md`, `docs/zh/31-...`, `docs/README.md`,
`docs/zh/README.md`, `examples/09-heterogeneous/boundary/README.md`,
`examples/09-heterogeneous/README.md`.

The island generator's section moves from 31 to 42, because 31's reader is a
rule author and no shipped rule calls the generator. 31 keeps one sentence and a
link. The name table in 42 gains the island rows. Both languages, with the
section numbering and the cross-references checked.

## T9 -- the check that keeps the two languages equal

`.github/tools/` already holds the documentation parity check that CI runs. The
new sections are added to both languages in the same commit, and the check is
run locally before the pull request.

## T10 -- release

Tag the release, publish it from the tag, `gtc` upload of the same bytes to
GitCode, then T6. This ran three times: 0.5.0 for the design, 0.5.1 for the
refusal of overlapping roots that the design states and 0.5.0 omitted, and
0.5.2 for the separator defect the first cross-platform run of the fixture
found.

## T11 -- ecosystem verification

In an xlings sandbox, with the CN mirror configured for both mcpp and xlings:
resolve `mcpp:plugins@0.5.0` from the index, build and run the `boundary`
example against the published package rather than a path override, and confirm
the qualified names in the generated module.

*Criterion:* the sandbox is the only thing that verifies the published artifact;
a path override in a working tree verifies the working tree.
