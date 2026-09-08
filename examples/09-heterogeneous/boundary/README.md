# The boundary, on its own — the ladder's lowest rung

The consumer imports a module that this build generated, and the project
contains no seam, no header, and no hand-written `.cppm`.

```
cd examples/09-heterogeneous/boundary
mcpp run                 # 6 12 18 24
```

Read this before [`../cuda`](../cuda/). It isolates the boundary between an
island and the C++ side; `cuda/` then adds the device compiler and the seam over
it, and the difference between the two is what each rung buys.

## The whole project

```
mcpp.toml                one dependency edge
build.mcpp               scan the root, emit the boundary
src/kernels/saxpy.c      the island
src/kernels/vec/scale.c  a second island, one directory deeper
src/main.cpp             import boundary.kernels;
```

`mcpp.tools.island` reads the entry points marked `MCPP_EXPORT_C` under the root
and writes two files into the build directory: the `extern "C"` header the
island reads, and a module over it whose whole content is a re-export.

```cpp
// generated
module;
#include "boundary.kernels.h"
export module boundary.kernels;

export namespace boundary::kernels {
using ::boundary_ran_on;
inline constexpr auto ran_on = boundary_ran_on;
using ::boundary_saxpy;
inline constexpr auto saxpy = boundary_saxpy;
}

export namespace boundary::kernels::vec {
using ::boundary_scale;
inline constexpr auto scale = boundary_scale;
}
```

Re-exporting names rather than restating signatures is what lets the generator
work without a C parser: it needs only the identifier before the `(`. It is also
why no second copy of a signature exists — at a C-linkage boundary that is the
copy that can disagree without anything noticing.

## The namespace is the module's own path, and a directory extends it

This is the rule the shader lane already followed and this generator did not
until `mcpp:plugins` 0.5.0.
[`docs/42`](../../../docs/42-heterogeneous-builds.md) states it once for both
lanes:

| written | reached as |
|---|---|
| `shaders/post/tonemap.frag` | `myapp::shaders::post::tonemap_frag()` |
| `boundary_scale` in `src/kernels/vec/scale.c` | `boundary::kernels::vec::boundary_scale` |

The leaf differs and the reason is visible in the table. A payload has no name
of its own, so the data lane derives one from the file name; an entry point
already carries one its author wrote, so the file name reaches nothing here. A
directory is the coarser unit either way, and moving a function between two
files in one directory renames nothing a consumer wrote.

**`saxpy` beside `boundary_saxpy`.** An island's symbol is global to the whole
program, so an entry point carries a package prefix whether or not it sits in a
namespace, and the namespace then repeats it. `options::strip_prefix` emits the
short spelling; the authored name is the symbol, is exported too, and is what
`nm`, a link error and a profiler show. The pair is one symbol — the short name
is a `constexpr` function pointer, not a second function.

**What the namespace does not buy.** C language linkage does not mangle, so two
entry points with one name are one symbol whatever namespace each appears in.
The generator therefore refuses two files in one root declaring one name, and
that refusal is what makes the namespace honest: a name exists in exactly one of
them.

## The island is C here, and that is the only simplification

An ordinary `.c` file, so this example runs on any machine with no device
toolkit. What makes it an island is the property the boundary exists for: it is
compiled separately, it cannot import a module, and its interface is
`extern "C"`. Substituting a `.cu` and a device rule is what `../cuda` shows,
and nothing about the boundary changes when that happens.

## Four rungs, and what each one costs

| rung | written by hand | the consumer writes | the consumer gets |
|---|---|---|---|
| **L0 — this example** | nothing but the marked entry points | `import boundary.kernels` | the island's own interface: pointers and a count |
| L1 — `../cuda`, `../sycl` | a seam module over the generated one | `import app.saxpy` | the interface the project designed |
| L2 | a seam, and entries built with `island::declared` | `import app.saxpy` | the same, for entry points a scan cannot see |
| L3 — `../hip`, `../vulkan`, `../cann` | the header and the module | `import app.saxpy` | the same, with the signature written twice |

**What L0 does not have.** The interface is C-shaped:
`boundary::kernels::saxpy(2.0f, x, y, out, 4)` rather than a span. And there is
no place for a
`cfg(accelerator = ...)` section to apply, because a seam is the single point at
which one implementation is exchanged for another. A project with one island and
one backend can stop here; a project that will swap backends needs L1.

**What the generator will not do.** Which functions, which types, and what
happens on failure are design decisions, so the C++ interface stays
hand-written. The generator produces what is mechanical around the entry
points — the include guard, the `extern "C"` block, the `__cplusplus` dance and
the module wrapper.

## Criteria

| criterion | measured |
|---|---|
| the consumer imports the generated module and names no header | `grep -rn '#include' src/` is empty |
| the project has no hand-written `.cppm` | the file list above |
| a directory below the root extends the namespace | `boundary::kernels::vec::scale` resolves, and `vec` appears in no source of this project |
| the short name and the authored one are one entity | `nm` on the artifact shows `boundary_saxpy` once and no `saxpy` |
| the result is the family's, halved by the second island | `6 12 18 24` |

The first two are why this example exists. `mcpp:plugins` records that a
consumer importing the generated module links against an implementation
compiled by a different driver, measured with GCC 16.1; until this example
nothing in either repository ran that arrangement — every fixture and every
example kept a seam on top.
