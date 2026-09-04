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

## The rule package, and why nvcc's host compiler is its problem

nvcc refuses host compilers newer than a bound it states in its own
`crt/host_config.h`, and mcpp's toolchain payload is routinely newer than that
bound. The rule reads the bound, selects a host compiler that satisfies it, and
says which one it chose:

```
example.rules.cuda: nvcc /usr/bin/nvcc with -ccbin /usr/bin/clang++-14
```

On the machine this example was verified on, the toolkit is CUDA 12.0
(`__GNUC__ > 12` is refused, clang must be below 15) and mcpp's payload is gcc
16.1.0, so passing mcpp's own compiler through would fail. `mcpp self doctor`
reports the same pairing independently.

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

## The one thing that is not closed yet

The CUDA runtime is linked statically, so the artifact carries every
redistributable component. That leaves exactly one host dependency,
`libcuda.so.1` — the driver's userspace library, which NVIDIA's licence forbids
redistributing and which is in ABI lockstep with the kernel module.

xim already has the right shape for this: `libcuda-host-link` is a sentinel
package that installs a symlink to whatever the host has, so every GPU consumer
reads one path instead of reimplementing an `ldconfig` probe. This example
declares it under `[xlings] deps`.

What is missing is the last hop. `[xlings] deps` provisions the payload and
puts its `bin/` on `PATH`, which is what a payload providing a *program* needs.
A payload providing a *library* also needs its `lib/` on the artifact's runtime
search path, and mcpp's private loader does not consult `/usr/lib`, so without
that hop the statically linked CUDA runtime cannot `dlopen` the driver and
reports it as "driver version is insufficient". Until an mcpp-index package
declares that directory, the run needs it supplied:

```
$ LD_LIBRARY_PATH=$(xlings pkginfo libcuda-host-link)/lib mcpp run
```

This is an ecosystem gap, not a defect in the design: the durable fix is a
`compat.cuda-driver` package in mcpp-index that wraps the sentinel and declares
its library directory, which is the same shape every other runtime provider in
that index already has.
