# 09 — System Graphics Without the Host

A KMS/DRM program that opens a GPU, allocates buffers and brings up EGL — with
nothing from `/usr/lib` and no host loader.

```bash
mcpp run
```

```
== the graphics stack, resolved from the index ==
  GBM_BACKENDS_PATH = …/subos/default/usr/lib/gbm
  wl_display_create        0x3f798be0
-- DRM node -> GBM device -> EGL display --
  /dev/dri/renderD128
  drm driver               nvidia-drm
  gbm_create_device        0x3f802f30
  gbm_bo_create            (driver declined this format/usage)
  eglGetPlatformDisplay    0x3f87d5b0
  eglInitialize            EGL 1.5, vendor Mesa Project
  /dev/dri/card0
  drm driver               simpledrm
  gbm_create_device        0x3f802f30
  gbm_bo_create            256x256 stride=1024 modifier=0xffffffffffffff
  eglGetPlatformDisplay    0x3f87d5b0
  eglInitialize            EGL 1.5, vendor Mesa Project
done.
```

That is one real run on a two-node machine, kept unabridged because the
difference between the nodes is the point: `simpledrm` allocated the buffer and
reported the driver's own stride and modifier, while NVIDIA's GBM backend
declined that format/usage combination. Both are the libraries answering — this
is the stack working, not a smoke test.

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
libdrm = "2.4.134"
libgbm = "25.0.7"
egl    = "1.7.0"

[target.'cfg(linux)'.dependencies.freedesktop]
wayland        = "1.26.0"
wayland-server = "1.26.0"
```

That is the entire configuration. `src/main.cpp` then includes `<gbm.h>`,
`<xf86drm.h>`, `<EGL/egl.h>` and `<wayland-client.h>` and calls the stock
upstream APIs — nothing in it is mcpp-specific, so code written against these
libraries anywhere else compiles here unchanged.

And it does the real thing rather than proving a symbol resolves: it opens a
DRM node, builds a genuine `gbm_device` from that fd, **allocates actual GPU
memory** with `gbm_bo_create` and reads back the stride and modifier the driver
chose, then hands the device to
`eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, …)` and initializes EGL. A
`gbm_create_device(-1)` on an invalid fd returns `NULL` and tells you nothing
about whether the stack works; this reaches `EGL 1.5, vendor Mesa Project`.

## Checking the claim

"Host-free" is easy to assert, so resolve the artifact's closure through the
private loader it actually uses and look at every path:

```bash
BIN=target/x86_64-linux-gnu/*/bin/graphics-stack
"$(readelf -p .interp $BIN | grep -o '/.*ld-linux[^ ]*')" --list $BIN
```

```
<project>/bin/libdrm.so.2                    <- built by this build
<project>/bin/libwayland-client.so.0         <- built by this build
<project>/bin/libwayland-server.so.0         <- built by this build
<project>/bin/libffi.so.8                    <- built by this build
compat-x-libgbm/25.0.7/…/libgbm.so.1
compat-x-egl/1.7.0/…/libEGL.so.1
xim-x-expat/2.6.2/lib/libexpat.so.1
xim-x-gcc/16.1.0/lib64/libgcc_s.so.1
xim-x-gcc/16.1.0/lib64/libstdc++.so.6
xim-x-glibc/2.44/lib64/libc.so.6
xim-x-glibc/2.44/lib64/libm.so.6
xim-x-libglvnd/1.7.0.1/lib/libGLdispatch.so.0
```

Nothing is under `/usr/lib` or `/lib64`. Two things there are worth reading
closely.

**The first four lines.** They are this project's own build output, not the
ecosystem's copies — including `libdrm.so.2`, even though Mesa's `libgbm.so.1`
has a DT_NEEDED on that soname and an absolute RUNPATH into the payload. The
consumer links them directly, so they are mapped first, and Mesa binds to them:
the `gbm_bo_create` above ran through this libdrm.

**`libffi` and `libGLdispatch`.** Nothing in `mcpp.toml` names either.
`libffi.so.8` is what `libwayland-client` dispatches protocol messages through,
`libGLdispatch` is what libEGL's vendor dispatch needs — the cascade a directly
linked library pulls behind it, which is exactly what a host `-L/usr/lib`
cannot resolve from inside a private loader.

## The packages

Three are built from source and two bind the ecosystem's Mesa, and the split is
not arbitrary. A library is built from source when upstream ships it as a
**separable unit**; it is bound when it is an internal build target of a project
the ecosystem already owns, where building it would mean forking that project.

| package | | what it gives you |
|---|---|---|
| `compat.libdrm` | source | `drmModeGetResources`, `drmModeAddFB2`, `drmModeSetCrtc` — the KMS side |
| `compat.libgbm` | binds `xim:mesa` | `gbm_create_device`, `gbm_bo_create` — buffers out of a DRM device |
| `compat.egl` | binds `xim:libglvnd` | `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, …)` — rendering onto them |
| `freedesktop.wayland` | source | `libwayland-client.so.0`, and `import wayland.client;` |
| `freedesktop.wayland-server` | source | `libwayland-server.so.0`, and `import wayland.server;` |

libdrm passes the test — an independent freedesktop project with its own
releases — so it is compiled here, five translation units with no dependencies
at all. GBM fails it: `src/gbm/meson.build` is `link_with: [libloader]`, and
`libloader` wants `idep_mesautil`, roughly 120 TUs of Mesa's internal utility
library for one function. It is also a *loader*, and the backends it dlopens
are Mesa's own, so built apart from Mesa it would have nothing to load.

Wayland passes it too, but needed more than a descriptor: its libraries are
mostly **generated** — `protocol/wayland.xml` describes every interface and
`wayland-scanner` emits ~13,000 lines from it — and the generator is a C program
in the same tree that must be compiled first. That does not fit an inline index
descriptor, so it lives in [mcpplibs/wayland](https://github.com/mcpplibs/wayland),
a fork that patches no upstream file. Client and server are two packages because
they are two distinct SONAMEs and Mesa's `libEGL_mesa` has DT_NEEDED on **both**.

**A payload carrying the same library is not a reason to bind**, which is worth
saying because it looks like one. Mesa's `libgbm.so.1` has a DT_NEEDED on
`libdrm.so.2` and an absolute RUNPATH into the payload's copy — and in this
program that RUNPATH loses. A soname already in the link map is reused, so
ld.so never searches for it again: the `libdrm.so.2` this project linked is
mapped first, exactly one is loaded, and Mesa's GBM allocated the buffer above
through it. That only holds because the package builds a *shared* library with
the canonical soname; merged into the consumer as objects there would be two
copies of libdrm's internal state over one set of file descriptors.

`compat.egl` is a binding for a duller reason: libglvnd IS separable, but
`libEGL.so` also needs its Python-generated dispatch stubs, `winsys_dispatch`
and the whole of `libGLdispatch.so`, so it has not been done yet.

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

**The wayland client and server are separate packages**, and this example asks
for both because it creates a `wl_display` on the server side. That is not a
packaging quirk: they are two SONAMEs, Mesa's `libEGL_mesa` carries DT_NEEDED on
each, and mcpp links every library target in a package against all of that
package's sources — so one package cannot emit two libraries with disjoint
contents. A client-only program drops the second line and links only
`libwayland-client.so.0`.

Both also ship a C++23 module wrapper. `import wayland.client;` in place of
`#include <wayland-client.h>` changes nothing else — every exported name is
upstream's, spelled upstream's way — so this file could switch one line at a
time. It uses the headers here because that is what a ported project looks like
on day one.

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
