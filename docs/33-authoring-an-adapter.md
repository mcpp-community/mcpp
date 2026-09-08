# 33 — Authoring a Runtime Adapter

**Reader:** someone making a library the **host** supplies reachable from an
mcpp-built artifact on Linux — a graphics driver, a Vulkan ICD, a proprietary
runtime.

**The question this chapter answers:** why an artifact cannot see a library that
is plainly installed on the machine, and what a package has to declare to fix
it.

**Not here:** packaging a library mcpp installs, which is
[32 — Authoring a Payload](32-authoring-a-payload.md) and is the right answer
whenever the library *can* be redistributed; and the runtime contract's fields,
which are [04 — The mcpp.toml Manifest](04-mcpp-toml.md) §`[runtime]`.

Before: [32 — Authoring a Payload](32-authoring-a-payload.md). After:
[34 — Authoring a Board-Support Package](34-authoring-a-bsp.md).

## The failure this exists for

The library is installed. The loader finds its manifest. The `dlopen` fails
anyway:

```
DRIVER: Found the following files: /usr/share/vulkan/icd.d/lvp_icd.json …
ERROR:  libvulkan_lvp.so: cannot open shared object file
```

The libraries are in `/usr/lib/x86_64-linux-gnu`. What cannot reach them is the
**process**: an mcpp-built binary runs under mcpp's own glibc, with its own
search path,

```
interp: …/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
rpath : …/xim-x-glibc/2.39/lib64:…/xim-x-gcc/…/lib64:$ORIGIN
```

so a bare-soname `dlopen` from inside that process does not search the host's
library path at all. Nothing is broken; the artifact is simply not looking
there, which is the property that makes an mcpp build reproducible in the first
place.

## The definition of an adapter

**A symlink farm plus the metadata that makes it reachable.** Nothing is
vendored, nothing is redistributed, and the package carries no upstream bytes.
`runtime.library_dirs` puts a package-owned directory of symlinks on the
artifact's runtime search path, and the chain resolves.

A project declares the adapter as an ordinary dependency and does nothing else.

## The reason the driver itself is not a package

A proprietary driver's userspace is in **ABI lockstep with a kernel module**,
and its licence forbids redistribution. Neither is a packaging problem that
effort solves, so such a driver is modelled as a **host capability** — something
the machine either has or does not — and the adapter is how an artifact reaches
it.

An **open** driver is a different case and takes the other answer: it is a
payload (`xim:mesa-lavapipe` for the CPU, `xim:mesa` for AMD hardware), and a
machine using one needs nothing from the farm. Since 2026.09.05 an adapter also
prefers the payload when one is published and its symbol set covers the host
copy, so what the farm actually records is proprietary userspace and packaging
backlog.

## The three things an adapter gets wrong

**The pattern list must cover transitive dependencies.** The whole chain has to
resolve through the same directory. Mesa's software rasteriser pulls in LLVM;
an NVIDIA driver pulls its own family. Listing the ICD alone produces the same
`cannot open shared object file` one level down.

**`libstdc++` belongs in the list, and it is not an oversight.** mcpp links
libstdc++ **statically** — it is absent from a built binary's `NEEDED` — so a
`dlopen`ed C++ driver has nothing to resolve against unless the host copy is
provided here.

**Nothing may be required.** A machine with no such driver at all is a
legitimate configuration, and every CI runner in this ecosystem is one. The farm
is then empty and the program reports what it actually found. An adapter that
errors on a missing host library turns a supported configuration into a build
failure.

## Current limitations

- **Linux only, by construction.** macOS's dyld and the Windows PE loader have
  no equivalent layer, so a project targeting them declares no adapter.
- An adapter cannot make a driver work that the machine does not have. It
  removes one obstacle — reachability — and reports the rest as absence.
- The farm's contents are decided when the adapter is installed. A driver
  installed afterwards is not picked up until the adapter is reinstalled.
