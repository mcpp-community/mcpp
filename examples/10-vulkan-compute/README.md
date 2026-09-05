# 10 — A Vulkan compute shader behind the same seam

The example next door compiles CUDA. This one compiles GLSL, dispatches it
through Vulkan, and prints the same four numbers — and the point is what the
two have in common, not what differs.

## The shape

```
app/
  shaders/scale.comp        the island: GLSL, compiled by glslang, never by
                            the C++ toolchain and never in the module graph
  src/vulkan/saxpy.cpp      the host side of the island: Vulkan calls, and the
                            SPIR-V compiled into the binary as a C array
  src/cpu/saxpy.cpp         the same interface implemented for the host,
                            compiled instead when the build names no device
  include/saxpy/saxpy.h     the island's interface: extern "C", no std types
  src/app.cppm              the seam: a module that turns the C interface back
                            into a C++ one
  src/main.cpp              an ordinary consumer, which imports the seam
  build.mcpp                hands the shaders to `mcpp.rules.spirv`, a member
                            of `mcpp:plugins` (mcpp-community/mcpp-plugins)
                            selected by the feature `rules-spirv`
```

## What is shared with `examples/09-cuda-kernel`

Everything structural. The device axis is declared once in the manifest, the
device sources are a constrained glob, the rule package receives them through
`MCPP_DEVICE_SOURCES`, and the engine never learns a vendor's name:

```toml
accel   = "vulkan1.2"
sources = [
  "src/*.cppm",
  "src/*.cpp",
  { glob = "shaders/*.comp", accel = "vulkan1.2" },
]
```

`vulkan1.2` is the SPIR-V target environment and reaches `--target-env` the
same way `sm_89` reaches `-gencode`. It carries no architecture set, and that
is not an omission: SPIR-V is the portable form, and which device executes it
is decided by the driver when the program runs. A rule that demanded an
architecture would be inventing a requirement its device API does not have.

## What is different, and what it demonstrates

**The output is a header, not an object.** A SPIR-V module is data the program
hands to `vkCreateShaderModule`. `mcpp.rules.spirv` submits its actions with
`role = "source"`, the one role the engine orders BEFORE compilation, and adds
the directory it writes to the include path. `#include "scale_comp.h"` then
resolves to a `const uint32_t scale_comp_spv[]`, and the artifact carries its
shaders — `mcpp pack` has nothing further to collect and the program does not
read a file at run time.

An `artifact` role would not do: its outputs are ordered against the LINK,
which is after the translation unit that includes the header is compiled.

**One artifact runs on every device.** The same binary was measured on three:

| where | how | output |
|---|---|---|
| NVIDIA RTX 4080 | the host's own ICD, through `compat.vulkan-runtime` | `12 24 36 48` |
| CPU (`llvmpipe`) | `xim:mesa-lavapipe`, a payload, with `VK_DRIVER_FILES` naming only it | `12 24 36 48` |
| no device at all | `mcpp build --no-accel`, the CPU implementation behind the seam | `12 24 36 48` |

The middle row is why the payload exists: a machine with no GPU — every CI
runner in this ecosystem — still has a Vulkan device, so the Vulkan lane is
something a test can assert on rather than something that only runs on a
developer's desk.

## Running it

```bash
cd examples/10-vulkan-compute/app
mcpp run                # whatever device the loader finds
mcpp run --no-accel     # the CPU implementation behind the same seam
```

The `[xlings.workspace]` block names the two payloads this build needs — the
shader compiler and the software driver — so the first build installs them.

## What this example does not show

Graphics. There is no swapchain, no window and no surface: the device API is
here for compute, which is the half a build system has to carry. A shader that
draws is compiled by exactly the same rule.
