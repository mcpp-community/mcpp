# The boundary, on its own — the ladder's lowest rung

The consumer imports a module that this build generated, and the project
contains no seam, no header, and no hand-written `.cppm`.

```
cd examples/09-heterogeneous/boundary
mcpp run                 # 12 24 36 48
```

Read this before [`../cuda`](../cuda/). It isolates the boundary between an
island and the C++ side; `cuda/` then adds the device compiler and the seam over
it, and the difference between the two is what each rung buys.

## The whole project

```
mcpp.toml                one dependency edge
build.mcpp               scan the island, emit the boundary
src/kernels/saxpy.c      the island
src/main.cpp             import boundary.kernels;
```

`mcpp.tools.island` reads the entry points marked `MCPP_EXPORT_C` and writes two
files into the build directory: the `extern "C"` header the island reads, and a
module over it whose whole content is a re-export.

```cpp
// generated
module;
#include "boundary.kernels.h"
export module boundary.kernels;

export using ::saxpy_device;
export using ::saxpy_device_name;
```

Re-exporting names rather than restating signatures is what lets the generator
work without a C parser: it needs only the identifier before the `(`. It is also
why no second copy of a signature exists — at a C-linkage boundary that is the
copy that can disagree without anything noticing.

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
| L2 | a seam, and the entry list passed to `emit` directly | `import app.saxpy` | the same, for entry points a scan cannot see |
| L3 — `../hip`, `../vulkan`, `../cann` | the header and the module | `import app.saxpy` | the same, with the signature written twice |

**What L0 does not have.** The interface is C-shaped: `saxpy_device(2.0f, x, y,
out, 4)` rather than a span. And there is no place for a
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
| the result is the family's | `12 24 36 48` |

The first two are why this example exists. `mcpp:plugins` records that a
consumer importing the generated module links against an implementation
compiled by a different driver, measured with GCC 16.1; until this example
nothing in either repository ran that arrangement — every fixture and every
example kept a seam on top.
