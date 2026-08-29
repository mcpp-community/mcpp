# 09 — System Graphics Without the Host

A KMS/DRM program that opens a GPU, allocates buffers and brings up EGL — with
nothing from `/usr/lib` and no host loader.

```bash
mcpp run
```

```
== the graphics stack, resolved from the index ==
  GBM_BACKENDS_PATH = …/subos/default/usr/lib/gbm
  wl_display_create        0x2e8d8be0
-- DRM node -> GBM device -> EGL display --
  /dev/dri/renderD128
  drm driver               nvidia-drm
  gbm_create_device        0x2e942f30
  eglGetPlatformDisplay    0x2e9bd390
  eglInitialize            EGL 1.5, vendor Mesa Project
done.
```

## What this example is for

System-level graphics work — a Wayland compositor, a Mesa-facing extension,
GBM buffer management — is the case people expect a managed runtime to be bad
at, because it is the case where "just link the system library" is the reflex.

mcpp's runtime deliberately does not depend on the host: that is what makes a
build reproducible and portable across distributions, and it is why an artifact
gets a private `PT_INTERP` whose search path mcpp computed rather than the
host's `/etc/ld.so.cache`. So `-L/usr/lib -lgbm` is not a thing mcpp is missing
support for — it is the wrong way to ask, and mcpp says so at build time.

The right question is whether the **work** can be done. This example is the
answer: the whole chain, done the recommended way, declaring dependencies.

```toml
[target.'cfg(linux)'.dependencies.compat]
libgbm  = "2026.08.29"
libdrm  = "2026.08.30"
egl     = "2026.08.30"
wayland = "2026.08.30"
```

That is the entire configuration. `src/main.cpp` then includes `<gbm.h>`,
`<xf86drm.h>`, `<EGL/egl.h>` and `<wayland-client.h>` and calls the stock
upstream APIs — nothing in it is mcpp-specific, so code written against these
libraries anywhere else compiles here unchanged.

And it does the real thing rather than proving a symbol resolves: it opens
`/dev/dri/renderD128`, builds a genuine `gbm_device` from that fd, hands it to
`eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, …)` and initializes EGL against a
real driver. A `gbm_create_device(-1)` on an invalid fd returns `NULL` and
tells you nothing about whether the stack works; this reaches
`EGL 1.5, vendor Mesa Project`.

## Checking the claim

"Host-free" is easy to assert, so it is worth resolving the artifact's closure
through the private loader it actually uses and looking at every path:

```bash
BIN=target/x86_64-linux-gnu/*/bin/graphics-stack
"$(readelf -p .interp $BIN | grep -o '/.*ld-linux[^ ]*')" --list $BIN
```

```
compat-x-egl/2026.08.30/…/libEGL.so.1
compat-x-libdrm/2026.08.30/…/libdrm.so.2
compat-x-libgbm/2026.08.29/…/libgbm.so.1
compat-x-wayland/2026.08.30/…/libwayland-client.so.0
compat-x-wayland/2026.08.30/…/libwayland-server.so.0
xim-x-expat/2.6.2/lib/libexpat.so.1
xim-x-gcc/16.1.0/lib64/libgcc_s.so.1
xim-x-glibc/2.44/lib64/libc.so.6
xim-x-glibc/2.44/lib64/libm.so.6
xim-x-libffi/3.4.4/lib/libffi.so.8
xim-x-libglvnd/1.7.0.1/lib/libGLdispatch.so.0
```

Every entry is under the registry; none is under `/usr/lib` or `/lib64`. Note
the bottom half especially — `libexpat`, `libffi` and `libGLdispatch` are
*transitive*: nothing in `mcpp.toml` names them. They are what a directly
linked `libgbm.so.1` cascades into, and resolving that cascade is exactly what
the host path cannot do from inside a private loader. Declaring the four
dependencies resolved all eleven.

## The packages

None of them vendors a source tree. Mesa, libdrm, libglvnd and wayland are
already in the ecosystem (`xim:mesa`, `xim:libdrm`, `xim:libglvnd`,
`xim:wayland`), so each package is a thin binding: it declares the ecosystem
package it needs and exposes that payload's headers and libraries to the
compiler. Building second copies would put two `libgbm.so.1` — or two
`libdrm.so.2`, or a second EGL dispatch library — in a process that already
loads Mesa's.

| package | what it gives you |
|---|---|
| `compat.libgbm` | `gbm_create_device`, `gbm_bo_create` — buffers out of a DRM device |
| `compat.libdrm` | `drmModeGetResources`, `drmModeAddFB2`, `drmModeSetCrtc` — the KMS side |
| `compat.egl` | `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, …)` — rendering onto them |
| `compat.wayland` | client and server libraries for the display protocol |

One honest gap, since "Mesa/Vulkan" usually get named together: Vulkan is not
part of this example and is not in the same state. `compat.vulkan-runtime`
builds its farm by harvesting the host's ICDs out of `/usr/lib/*` and `/lib64`,
because a Vulkan driver is the GPU vendor's and there is no ecosystem payload
to bind to yet. The GBM/KMS/EGL/Wayland stack above has no such edge.

## Two things worth knowing

**`GBM_BACKENDS_PATH` is not set by any of these packages.** libgbm is a
loader: `gbm_create_device()` dlopens `<path>/<driver>_gbm.so`, and the path
Mesa compiles in is `/usr/lib/gbm` — correct on a distribution, wrong the
moment the payload lives anywhere else. Setting that variable is Mesa's own
mechanism and the *environment's* job, which is where every relocated stack
puts it (Valve's pressure-vessel, Nix, Conda all do exactly this). Here
`xim:mesa` declares it into the SubOS and mcpp carries SubOS declarations into
the processes it launches, so it is simply already set — which is why the
program prints it rather than computing it.

**`compat.wayland` puts only `-lwayland-client` on the link line**, and this
example adds the other half itself:

```toml
[target.'cfg(linux)'.build]
ldflags = ["-lwayland-server"]
```

A dependency's `ldflags` reach every consumer with no way to opt out, so a
package that forced `libwayland-server` on every client would be unfixable
downstream. All four wayland libraries are present; a compositor asks for the
one it needs and it resolves out of the same package.

## Running it

The DRM section needs a GPU. On a machine without one — a container, most CI
runners — the program says so and everything that does not need hardware has
already run:

```
  (no DRM node reached EGL — expected without a GPU)
```

To watch the backend loader itself, ask Mesa:

```bash
EGL_LOG_LEVEL=debug mcpp run
```
