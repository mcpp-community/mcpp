# HIP: a kernel on the NVIDIA platform, behind the same seam

What this example demonstrates, and what it does not.

## The shape

```
app/
  src/kernels/saxpy.hip   the island: a device translation unit written against
                          the HIP API
  src/cpu/saxpy.cpp       the same interface implemented for the host
  include/saxpy/saxpy.h   the island's interface: extern "C", no std types.
                          WRITTEN BY HAND, and that is why this example
                          exists beside `cuda/` -- see below
  src/app.cppm            the seam
  src/main.cpp            an ordinary consumer
  build.mcpp              hands the device sources to `mcpp.rules.hip`
```

## The boundary is written by hand here, and generated in `cuda/`

The two examples compute the same thing through the same seam, so the one
difference between them is how the `extern "C"` boundary comes to exist.

Here it is a file in the source tree. Each signature appears twice -- once in
`include/saxpy/saxpy.h` and once at each definition -- and nothing checks that
the copies agree. That is the ordinary arrangement, and it is worth seeing
written out, because it is what every C boundary looks like and because the
failure it permits is quiet: C language linkage does not mangle and the island
and the CPU fallback are never in one link, so a signature that drifted produces
a clean build and an artifact that reads its arguments by whichever version it
was compiled with.

`cuda/` marks the entry points where they are defined and lets
`mcpp.tools.island` write the header and the module from them. The signatures
then exist once, and the generator -- handed both implementations -- refuses a
disagreement at the one point where both texts are in front of it.

Neither is deprecated. A project whose boundary is stable, or whose island is
compiled somewhere mcpp cannot reach, writes the header; the generated form is
the default because the copy it removes is the one that goes wrong silently.

## The kernel

Compare `src/kernels/saxpy.hip` with example 09's `saxpy.cu`: the same kernel,
the same seam, and every device call spelled `hip*` instead of `cuda*`.

## HIP on this platform is a header layer, not a second runtime

HIP has two implementations behind one API. On AMD hardware it is a runtime
library that talks to ROCm. On NVIDIA hardware every entry point is an inline
wrapper over the CUDA one — `hipMalloc` resolves to `cudaMalloc` through the
header, `hipError_t` is `cudaError_t` under a typedef — so the object links
against the CUDA runtime and nothing of ROCm's.

Three consequences, all visible in this example's manifest:

* the compiler is the project's own clang, the same one that compiles the C++
  half, invoked `-x cuda` with `-D__HIP_PLATFORM_NVIDIA__`;
* `xim:hip-nvidia` contains no binaries, because on this platform there are
  none to contain;
* the payloads are CUDA's, and `[dependencies.compat] cuda-driver` is the same
  one hop example 09 needs — mcpp's private loader does not consult `/usr/lib`,
  so the statically linked CUDA runtime could not otherwise `dlopen` the driver.

One of those payloads, `xim:cuda-profiler-api`, is here because CI found it
missing. `nvidia_hip_runtime_api.h` includes `<cuda_profiler_api.h>` on its
second line and CUDA ships that header in a separate component, so a machine
with a host CUDA installation supplies it from `/usr/include` and the build
works while depending on something it never declared.

`hipcc` is deliberately not used. It is a driver that reads `HIP_PLATFORM`,
picks nvcc or amdclang and forwards; every decision it makes is one the rule
has already made from the declaration, and it would make them again from the
environment.

## The device is spelled once

```toml
[build]
accel = "hip, cuda12.9+{sm_89}"
```

Two chunks. The first names the programming model, the second names the device
— and the second is character for character what example 09 writes. A device
has one spelling in this ecosystem however many programming models reach it, so
`sm_89` does not acquire a second one because the file is called `.hip`.

## The AMD platform

Refused, by name, with the reason. It needs a ROCm runtime and device library
that this ecosystem does not publish yet, so compiling for it would produce an
object nothing on the machine can link or run. That refusal is the honest
answer; the alternative is a build that succeeds and an artifact that does not.

## Running it

```
mcpp build                 # sm_89, through mcpp.rules.hip
mcpp run                   # 12 24 36 48, on the device
mcpp build --no-accel      # the constrained glob is left out
mcpp run --no-accel        # 12 24 36 48, from src/cpu/saxpy.cpp
```
