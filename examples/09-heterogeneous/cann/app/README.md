# Ascend, through the same island

`op_kernel/` beside `op_host/` is how CANN's own operator libraries are already
laid out — every operator in `ops-math` splits that way on disk. The island is
therefore not a shape mcpp imposes on Ascend; it is the shape Ascend already
has, and this example writes it in mcpp's vocabulary.

`.asc` is that seam made checkable, exactly as `.sycl` is in the SYCL example:
the file's content is C++ and nothing in it would tell a reader otherwise. What
makes it a device translation unit is that it goes to BiSheng — a compiler with
a device back end, and one that does not accept C++20 modules.

## What builds, and what does not

```bash
mcpp run --no-accel                        # builds and runs
mcpp build --accel "ascend8.5+{dav-c220}"  # compiles the kernel, links, then
                                           # stops on the missing driver
```

Measured on an x86_64 machine with **no Ascend hardware and no Ascend driver**:

| step | result |
|---|---|
| `xim:cann-toolkit` provisioned | 2.9 GB, no root, no driver |
| `build.mcpp` compiles and runs | `mcpp.rules.ascendc` imported from `mcpp:plugins` |
| the kernel compiles | `bisheng -x asc --cce-aicore-arch=dav-c220` |
| the object joins the ordinary link | mixed mode: an x86-64 object carrying the device binary |
| the host half links | ACL, plus the six-library closure the rule names |
| the artifact starts | **no** -- `libascend_hal.so` is missing |
| `--no-accel` | builds and runs: `12 24 36 48`, `device: cpu` |

`libascend_hal.so` belongs to the **driver**, not the toolkit, and is the role
`libcuda.so.1` plays for CUDA: in ABI lockstep with the kernel module, not
redistributable, and absent on a machine with no NPU. A device build of this
example therefore completes everywhere and *runs* only on an Ascend machine --
which is the same statement `examples/09-heterogeneous/cuda` makes about a
machine with no NVIDIA driver, and the reason both are skipped by CI.

The payloads are gated on the accelerator, so `mcpp run --no-accel` installs
nothing: the CPU leg costs a C++ compile and no download at all.

## What the toolkit turned out to be

The design this follows expected the toolkit to be hard to obtain. It is not.
Every CANN toolkit from 8.0.RC1 to 8.5.0 is a plain `.run` on Huawei's own OBS,
answering 200 to an anonymous HEAD request, and it installs unattended:

```bash
./Ascend-cann-toolkit_8.5.0_linux-x86_64.run --install \
    --install-path=<dir> --quiet
```

Both halves the lane needs are inside it:

| | |
|---|---|
| `<arch>-linux/ccec_compiler/bin/{ccec,bisheng}` | the device compiler, clang 15.0.5 |
| `<arch>-linux/simulator/<SoC>/lib/libpem_davinci.so` | **38 SoCs**, no hardware required |

The second is why the lane is verifiable without an NPU at all, and it is the
next thing this example should use: the kernel compiles today, and running it
under `libpem_davinci` is a separate piece of work with its own contract.

## The seam is a C function, and that is measured rather than stylistic

BiSheng's own launcher for a `__global__` function is **C++-mangled** even when
the kernel is declared `extern "C"`. Calling it from the host half would make
this program depend on BiSheng and the project's C++ compiler agreeing about
mangling -- two different compilers, one of them clang 15 and the other
whatever the project chose. The `.asc` file therefore exports an `extern "C"`
wrapper, and the `<<<...>>>` launch spelling never leaves the translation unit
the device compiler owns.

## Why `accelerator = "none"` for the fallback

`not(accelerator = "ascend")` would work today and rot tomorrow: `accelerator`
is an open vocabulary, so a fallback written by enumerating what it is not
changes meaning every time the ecosystem gains a backend. Ascend is itself an
instance of that growth, which is the neatest possible argument for the
spelling.
