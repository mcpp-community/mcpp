# SYCL: a kernel behind the same seam, compiled by a second compiler

What this example demonstrates, and what it does not.

## The shape

```
app/
  src/kernels/saxpy.sycl  the island: a device translation unit, never scanned,
                          no BMI, and compiled by a SECOND compiler
  src/cpu/saxpy.cpp       the same interface implemented for the host, compiled
                          instead when the build asks for no accelerator
  include/saxpy/saxpy.h   the island's interface: extern "C", no std types
  src/app.cppm            the seam: a module that turns the C interface back
                          into a C++ one
  src/main.cpp            an ordinary consumer, which imports the seam and
                          never sees the header
  build.mcpp              hands the device sources to `mcpp.rules.sycl`, a
                          member of `mcpp:plugins` selected by `rules-sycl`
```

Everything above is the shape of example 09, one file name apart. That is the
point of this example: SYCL is a different compilation model and it reaches
the build through the same seam, the same constrained glob and the same rule
mechanism.

## What makes a `.sycl` file a device unit

Not its content. Open `src/kernels/saxpy.sycl` and it is ordinary C++ — there
is no dialect to see, no `__global__`, no launch syntax. What makes it a device
translation unit is that it goes to a compiler with a device back end, which
mcpp does not drive and which does not accept C++20 modules. `SourceKind::Device`
states exactly that property and nothing about the language.

Naming it `.cpp` and routing it by glob was possible and was rejected: one
extension would then mean two things depending on which glob matched first, and
the seam is legible precisely because the file name says which side of it a
unit is on.

## Two edges, not one per source

A SYCL object carries its device image, and nothing registers that image with
the runtime. The registration comes from a **device link** (`-fsycl-link`),
which reads every device object and emits one further host object. So the rule
submits one action per source and one that consumes their outputs, and the
engine orders them by the graph rather than by declaration order.

Without the second, this program links, starts, and finds no kernel.

## Three payloads, and what each one closes

```toml
[xlings.workspace]
"xim:dpcpp"     = "7.1.0"      # the compiler: its clang has the SYCL front end
"xim:gcc"       = "15.1.0"     # the C++ standard library the unit compiles against
"xim:cuda-nvcc" = "12.9.86"    # the NVIDIA back end's libdevice
```

`xim:gcc` is not a second toolchain. Left alone, the SYCL compiler takes its
C++ standard library headers from the host's GCC, and `--cuda-path` aside, it
finds the host's CUDA installation the same way. Neither says anything when it
happens: both are visible only in the compiler's own include search list, and
only on a machine that has those directories. That is why the rule refuses
without them and names the line to add.

## Two C++ runtimes, and why the seam is not optional here

`libsycl.so` is compiled against libstdc++ while an mcpp artifact links libc++,
so both are in the image. mcpp's duplicate-symbol check reports the unwinder
symbols they share, and the warning is correct.

Nothing may cross the seam. The island catches its own `sycl::exception` and
returns a code, because the runtime that threw it is not the one the caller
would unwind with. Example 09's island can promise not to touch the standard
library at all; this one cannot — SYCL *is* a C++ library — so the discipline
moves from "no standard library" to "nothing crosses".

Making that promise true took three things, and only two of them are a `catch`:

* the catch sits **inside** the buffer scope, because a `sycl::buffer`
  destructor blocks until the work that reads it has finished, and unwinding
  through three of those is a second throw during unwinding;
* the queue takes an **asynchronous handler**, because a queue constructed
  without one gets the default handler, and the default handler calls
  `std::terminate` — which no `catch` can intercept, since it never travels as
  an exception through this frame;
* and one failure remains outside both. A build compiled to SPIR-V, run against
  a back end that does not consume it, throws from inside the SYCL scheduler.
  That is why this manifest names the device, and why `mcpp.rules.sycl` warns at
  build time when an `accel` names `sycl` and no device.

## Running it

```
mcpp build                 # ahead of time for sm_89, through the dpcpp payload
mcpp run                   # 12 24 36 48, on the device
mcpp build --no-accel      # the constrained glob is left out
mcpp run --no-accel        # 12 24 36 48, from src/cpu/saxpy.cpp
```

A machine with no device runs the second pair and gets the same answer. That is
what the seam buys, and it is the same sentence example 09's README ends with.
