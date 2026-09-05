# 09 — A CUDA kernel behind a seam module

What this example demonstrates, and what it does not.

## The shape

```
app/
  src/kernels/saxpy.cu    the island: compiled by nvcc, never scanned, no BMI
  include/saxpy/saxpy.h   the island's interface: extern "C", no std types
  src/app.cppm            the seam: a module that turns the C interface back
                          into a C++ one
  src/main.cpp            an ordinary consumer, which imports the seam and
                          never sees the header
  build.mcpp              names the sources and the architectures
rules-cuda/               a build-rule package that knows how to run nvcc
```

Three properties are load-bearing.

**The island is not in the module graph.** No device compiler accepts C++20
modules, so `.cu` is classified as a device translation unit: never scanned for
imports, never producing a BMI. Its header is classified as a header, so
editing one still invalidates the fast path.

**The island's interface is `extern "C"` and free of standard-library types.**
nvcc drives a host compiler that mcpp did not choose, so the two sides do not
share a C++ ABI and must not exchange anything that depends on one. The island
also uses no standard library itself, which keeps it from linking a second copy
of the C++ runtime into a program whose own copy came from mcpp's toolchain.

**The seam exists for backend substitution, not for the module boundary.** It
is the one place where the island underneath could become HIP or a CPU
fallback without any consumer of `app.saxpy` changing, and the one place a
`cfg(accelerator = ...)` section has to apply. Remove it and every importer
becomes backend-specific.

## Where the toolkit comes from

The project names it:

```toml
[xlings.workspace]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"
```

These are payloads, so the version is the project's choice and not the machine's.
The rule package resolves them with `mcpp::xpkg_dir` and builds the whole
invocation from what it finds — the compiler, the include directories and the
library search paths. **No path in this example is absolute**, and a build here
touches nothing of the host's CUDA:

```
$ mcpp build -v | grep -c '/usr/local/cuda\|/usr/bin/nvcc'
0
```

Host locations remain in the rule as a last fallback, so a machine that has only
a distribution toolkit still builds. They are a fallback, not the design.

⚠️ **The payload's headers have to be named.** nvcc adds
`<its own directory>/../include` by itself, and on the 12.x line that holds
`crt/` but not `cuda_runtime.h` — which lives in the `cuda-cudart` component. An
earlier revision of this rule left it out, and nvcc resolved `cuda_runtime.h`
from `/usr/include` and then read the **host's** `crt/host_config.h` beside it.
The build failed with the host toolkit's complaint while using the payload's
compiler.

## The rule package, and why nvcc's host compiler is its problem

nvcc refuses host compilers newer than a bound it states in its own
`crt/host_config.h`, and mcpp's toolchain payload is routinely newer than that
bound. The rule reads the bound — from the payload, which states a newer one
than a distribution toolkit does — selects a host compiler that satisfies it,
and says which one it chose:

```
example.rules.cuda: nvcc …/xpkgs/xim-x-cuda-nvcc/12.9.86/bin/nvcc with -ccbin …
```

`mcpp self doctor` reports the same pairing independently, and reads the same
payload.

Everything about nvcc's spelling lives in the rule package. The engine owns the
graph, the artifact's identity and the architecture set; it does not own
`-gencode`.

## Verified

On an NVIDIA RTX 4080 (compute capability 8.9) with CUDA 12.0 and driver
550.144.03:

```
$ mcpp run
     Running `target/.../bin/cuda-saxpy`
12 24 36 48
```

which is `2.0 * [1,2,3,4] + [10,20,30,40]` computed on the device.

## Where the driver comes from

The CUDA runtime is linked statically, so the artifact carries every
redistributable component. That leaves exactly one host dependency,
`libcuda.so.1` — the driver's userspace library, which NVIDIA's licence forbids
redistributing and which is in ABI lockstep with the kernel module.

Reaching it takes two packages, one per layer.

`libcuda-host-link` in xim owns the question of *where* the host's copy is. It
installs a symlink to whatever the machine has, so every GPU consumer reads one
path instead of reimplementing an `ldconfig` probe.

`compat.cuda-runtime` in mcpp-index owns the mcpp-side question of how a
**built artifact** reaches it. mcpp's private loader does not consult
`/usr/lib`, so a bare-soname `dlopen` from inside a built program finds nothing;
the package declares a directory on the artifact's runtime search path and links
through the sentinel. It is declared here as an ordinary dependency:

```toml
[dependencies.compat]
cuda-runtime = "2026.09.05"
```

Without it the program builds and links, then reports

```
cudaMalloc: CUDA driver version is insufficient for CUDA runtime version
```

which is what the runtime says when it cannot open the driver at all.
