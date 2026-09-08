# 10 — Graphics: a pipeline whose output can be asserted

`offscreen/` renders a triangle through a real Vulkan graphics pipeline —
vertex input, rasterisation, fragment output, render pass — into an image, reads
the pixels back, and checks them. It is the graphics counterpart of
[`09-heterogeneous/vulkan`](../09-heterogeneous/vulkan), which does the same for
compute.

```
cd examples/10-graphics/offscreen
mcpp run                 # on whatever Vulkan device the loader finds
mcpp run --no-accel      # the same image, from a software rasteriser
```

## Why offscreen rather than a window

A swapchain needs a surface, and a surface needs a window system. On a headless
machine there is none, so an example built around one can only be **built**
there — and "it built" says almost nothing about a graphics pipeline. A fragment
shader that ignores its input, a pipeline whose vertex stage never runs, an
image never rendered into: all three compile and link.

Rendering into an image makes the result assertable, and it exercises the same
pipeline a windowed application uses. What a window would add is presentation,
which is the one part that cannot be checked without one.

## What the assertions are

| | |
|---|---|
| the four corners | exactly the clear colour, with no tolerance — nothing interpolates there, and a tolerance would hide an image that was never rendered into |
| the centre | inside the triangle, so all three vertex colours contribute. **Every channel must be non-zero** |
| the device name | printed after the run, never before |

The centre is the assertion this example exists for. A fragment shader writing a
constant would put 255 in one channel and 0 in the other two; a pipeline whose
vertex stage never ran would leave the clear colour. Requiring all three
channels to be non-zero separates those from an interpolated result **without
depending on a rasteriser's exact rounding**.

Measured: both legs produce `(124, 70, 62, 255)` at the centre — the same bytes
from llvmpipe and from the software rasteriser in `src/cpu/`.

## The two implementations behind one seam

`src/vulkan/render.cpp` and `src/cpu/render.cpp` define the same three
`extern "C"` entry points and are never in one link. They produce the same
image on purpose: **the pixel test is the contract**, and the CPU leg exists to
show the contract is satisfiable without a GPU. `render_device_name()` is what
tells the two apart, which is why it is printed.

## What the build system contributes

```toml
[build-dependencies.mcpp]
plugins = { version = "0.5.0", features = ["rules-spirv"], host-module = true }

[build]
accel = "vulkan1.2"
sources = [
  "src/*.cppm", "src/*.cpp",
  { glob = "shaders/*.vert", accel = "vulkan1.2" },
  { glob = "shaders/*.frag", accel = "vulkan1.2" },
]
```

That is the whole of it. `mcpp.rules.spirv` declares the shader compiler it
drives, so this project names no payload for it, and the constrained globs route
the shaders to the build program rather than to the C++ compiler.

The compiled stages arrive as a MODULE. `build.mcpp` asks for that surface in
one line, and `src/vulkan/render.cpp` writes `import offscreen.shaders;` and
calls `offscreen::shaders::triangle_vert()`. It used to write
`#include "triangle_vert.h"` and `#include "triangle_frag.h"` -- two names no
line in this project produced and no reader could derive without opening the
rule. The accessor answers with the address and the byte count together, which
is what makes `sizeof` the wrong question rather than an awkward one: under
object storage there is no array to take the size of.

**A dependency cannot be conditioned on the accelerator, and this project is
where that shows.** `accelerator` is resolved from the dependency graph, so a
dependency chosen by it would decide the answer it is asking for. mcpp says so
and ignores the predicate. An earlier revision of this manifest gated the Vulkan
loader on `cfg(accelerator = "vulkan")` and the build failed on
`vulkan/vulkan.h: No such file or directory` — the header's package had been
dropped while the source that includes it, selected by the same predicate, was
kept. Packages are therefore unconditional or conditioned on the platform;
`[build]` sources are what the accelerator selects.

**A portability driver is hidden until the program asks for it.** macOS has no
native Vulkan: MoltenVK implements it on top of Metal, and the specification
calls such an implementation a *portability driver*. The loader does not show
one to `vkEnumeratePhysicalDevices` unless the instance enables
`VK_KHR_portability_enumeration` and sets
`VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`; a device that advertises
`VK_KHR_portability_subset` must then have that extension enabled at
`vkCreateDevice` or the call fails. A program written against a native driver
alone therefore finds no device on such a machine and reports it as "no GPU
here", which is the wrong diagnosis.

`src/vulkan/render.cpp` asks the loader and the device what they advertise
rather than testing for the platform. The property is "the loader in front of
me is showing portability drivers", and an `#ifdef __APPLE__` would be wrong in
both directions: a Linux machine running a translation layer has it, and a
macOS build against a native driver does not need it.

**One shader per stem.** The generated name is the shader's stem and its stage,
so `ui/text.vert` and `world/text.vert` would both produce `text_vert.h`
declaring `text_vert_spv`. The rule refuses that and names both files. The
directory cannot be part of the name: two headers reaching one translation unit
would still collide on the symbol.
