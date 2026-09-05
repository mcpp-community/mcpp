# 12 — A HIP kernel on the NVIDIA platform

What this example demonstrates, and what it does not.

## The shape

```
app/
  src/kernels/saxpy.hip   the island: a device translation unit written against
                          the HIP API
  src/cpu/saxpy.cpp       the same interface implemented for the host
  include/saxpy/saxpy.h   the island's interface: extern "C", no std types
  src/app.cppm            the seam
  src/main.cpp            an ordinary consumer
  build.mcpp              hands the device sources to `mcpp.rules.hip`
```

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
* the payloads are CUDA's, and `[dependencies.compat] cuda-runtime` is the same
  one hop example 09 needs — mcpp's private loader does not consult `/usr/lib`,
  so the statically linked CUDA runtime could not otherwise `dlopen` the driver.

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
