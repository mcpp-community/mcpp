# 20 — Accelerators

How mcpp builds device code, and how a prebuilt artifact states which devices
it can run on.

## Two shapes, one of which mcpp implements today

Accelerator toolchains come in two shapes, and they are not variations of one
model.

An **island** keeps device code in separate translation units compiled by a
separate compiler. CUDA, HIP, Ascend C and Metal all work this way. The device
compiler produces an object (or, for shading languages, a runtime resource)
that joins the ordinary link.

A **whole-target** model puts device code in ordinary `.cpp` files and compiles
the entire target with a compiler capable of offloading. SYCL, OpenMP offload
and stdpar work this way. There is no island to separate.

This document describes the island shape, which is what mcpp implements.

## Device translation units

A source whose extension is `.cu` or `.hip` is a **device translation unit**.
mcpp classifies it as such and treats it accordingly: it is never scanned for
imports and never produces a BMI, because no device compiler accepts C++20
modules.

`.cuh` and `.hiph` are classified as headers. They are not compiled, but
editing one can change what the graph should be, so they invalidate the fast
path exactly as any other header does.

Device extensions are **not** in the default source glob. A package that
vendors a `.cu` it builds elsewhere must not begin compiling it on an mcpp
upgrade, which is a break its author cannot fix once that version has shipped.
Device sources are opted into by naming them.

## The seam

A device translation unit cannot import a module, so the boundary between it
and the rest of a project is a header. Consumers do not see that header: a
module includes it in its global module fragment and exports a C++ interface,
and everything downstream imports the module.

That module is worth naming a *seam*, because its reason for existing is not
the module boundary. It is the single place where the island underneath can be
exchanged — for HIP, for a CPU fallback — without any consumer changing, and
the single place a `cfg(accelerator = ...)` section has to apply. A project
without a seam has no boundary at which a backend can be substituted.

Two constraints on the interface follow from what the compilers are, not from
taste. It should be `extern "C"`, because the device compiler drives a host
compiler that mcpp did not choose and the two sides therefore do not share a
C++ ABI. The island should avoid the standard library, because an island that
links libstdc++ puts a second copy of the C++ runtime into a program whose own
copy came from mcpp's toolchain.

## Compiling an island

The command that invokes a device compiler is not built into mcpp. It is
supplied by a **build-rule package**, consumed with `host-module = true`,
which emits build-graph edges whose outputs join the link. See
[07 — build.mcpp](07-build-mcpp.md) for the mechanism and
`examples/09-cuda-kernel` for a working CUDA rule.

The division is deliberate. mcpp owns the graph, the artifact's identity and
the set of architectures; a vendor's flag spelling, its architecture syntax and
its host-compiler requirements belong to the rule.

## What the rule package reports before the first compile

Three things go wrong late with a device toolkit, and none of them is a fact
about the build graph. They are read and reported by the **rule package** that
drives the tools -- `examples/09-cuda-kernel/rules-cuda` shows each one -- and
the engine owns none of them (`tests/unit/test_core_vendor_probes.cpp` holds
that line, so a second backend never grows a second copy inside mcpp).

**The host compiler a device compiler will accept.** nvcc refuses host
compilers newer than a bound it states in its own `crt/host_config.h`, and
mcpp's toolchain payload is frequently newer than that bound. On the nvcc
route the rule reads the bound from the toolkit it resolved -- a payload
before the host, because a toolkit installed through xlings is the one the
build uses and is usually the newer one (a 12.9 payload states `gcc <= 14`
where a distribution's CUDA 12.0 states `gcc <= 12`) -- and says which
compiler it chose and why, through `mcpp::warning`. The primary route has no
such bound: `clang -x cuda` is its own host compiler.

**Whether the device compiler can reach its own back-end.** A toolkit can be
installed, complete and on `PATH` and still fail at its first stage: nvcc
runs `cicc`, `cudafe++`, `ptxas` and `fatbinary` as bare names on a `PATH` it
prepends from an `nvcc.profile` beside its own binary, and a sandbox that
replaces `/etc` removes a Debian-packaged profile. The rule asks nvcc for its
plan (`nvcc --dryrun`) rather than assuming one, resolves each stage, and
names the first one that does not resolve together with the payload that
provides it. A dryrun that produces no plan yields no finding.

**Whether the driver is new enough for the runtime.** A device runtime must
not be newer than the driver it runs against; when it is, the build compiles
and links cleanly and fails at the first allocation with *"CUDA driver
version is insufficient for CUDA runtime version"*. The rule reads the
driver's version through the driver's own library (reached through the
sentinel package, never through `/usr/lib`) and states it as a fact; it states
the floor its runtime needs; and the engine compares the two before anything
is compiled -- see the probe channel in [07 — build.mcpp](07-build-mcpp.md).
The engine reads a name, a relation and a version; `cuda.driver` is data
flowing through.

Reported rather than enforced where a wrong answer would cost more than none:
a machine with no rule package in its project has nothing vendor-specific to
say and says nothing, and a probe that reaches no answer invents none.

## Declaring what a build targets

```toml
[build]
accel = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

overridden for one build by `--accel`, which is the relationship `--target`
has with `[toolchain]`. `--no-accel` is not the absence of `--accel`; it is an
explicit request for no accelerator, which is what selects a CPU-only variant
of a package that also publishes device builds.

A source package may declare which backends it supports:

```toml
[package]
accelerators = ["cuda", "rocm"]
```

This mirrors `[package] platforms`: a statement of intent and a CI-matrix hint,
not a gate. It is a different field from an artifact's `accel` on purpose — a
declaration is written by hand and may be aspirational, while an artifact's
field is measured from the build that produced it.

## What a prebuilt artifact states

An artifact that carries device code records it beside its compatibility tag:

```toml
[[runtime.artifacts]]
role       = "static-library"
path       = "lib/libgpukit.a"
provenance = "mcpp-pack/1"
abi        = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
accel      = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

The field is separate from the tag rather than a segment of it because an
architecture list is a set, and the tag is a dash-joined string whose triple
already contains a variable number of dashes.

An absent `accel` means the artifact carries no device code and constrains
nothing, which is why a CPU-only library is usable by every build.

### How a consumer is matched

A build's request is satisfied by an artifact when, for each backend the build
asks for, the artifact declares that backend, agrees on the toolkit's major
version, and covers every requested architecture. An architecture is covered
when it is named, when a family target of the same major and an equal-or-lower
minor is named, or when the embedded portable form's floor is at or below it.

Family targets and portable forms are what keep the variant matrix finite.
Publishing one artifact per chip does not scale; publishing one per generation
does.

### The grammar is open

`cuda`, `hip`, `vulkan` and `sycl` are not a closed set. A backend name, a
version, an architecture list and an optional floor are the whole shape, and
mcpp compares them without a table of who exists:

```
vulkan1.3+{spirv1.6} floor>=1.4
sycl2020+{spir64,nvptx64-sm_89}
hip6.4+{gfx942}
```

An architecture whose spelling carries no leading number — `gfx942` — is
compared by equality, because there is no ordering to read out of it. That is
the answer rather than a guess: reading `942` out of the middle would invent a
level AMD does not define.

`floor>=` is the backend-neutral spelling of the portable-form floor. `ptx>=`
is CUDA's word for the same field and remains accepted, so descriptors written
before this are unaffected; a backend whose portable form is SPIR-V writes
`floor>=` instead of borrowing NVIDIA's term.

Which spellings mean what for a given backend is the business of that backend's
rule package. What the engine holds is the shape and the comparison.

When nothing matches, the refusal names the dimension and both sides:

```
error: mcpplibs.gpuonly@0.1.0: no prebuilt artifact matches this toolchain.
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23  accel=cuda12.8+{sm_86}
  published tags :
                   x86_64-linux-gnu  accel=cuda12.8+{sm_90f}
  closest is x86_64-linux-gnu, and it differs on:
    accel     needs cuda12.8+{sm_90f}, this build has cuda12.8+{sm_86}
  fix: build for an architecture the package carries (--accel), or take
       a variant that carries no device code (--no-accel), or ask the
       publisher for one covering yours.
```

This is the failure the dimension exists to move. Without it the build links
cleanly and the program fails at its first kernel launch with a message naming
neither the package nor the architecture either side expected.

### Publishing several variants

A package may publish several artifacts, and a consumer takes the first whose
tag accepts it. **List the CPU-only artifact first.** An mcpp that predates the
`accel` field ignores it, and ordering is what still gives such a client an
artifact that runs anywhere.

## Conditioning on the backend

`accelerator` is a target-side layer, resolved after the dependency graph is,
and it holds a set rather than a single value:

```toml
[target.'cfg(accelerator = "rocm")'.build]
cxxflags = ["-DMYAPP_ROCM"]
```

The comparison is membership, so a build enabling both CUDA and ROCm answers
true to each. `any`, `all` and `not` compose over it as ordinary boolean
combinators.

## Two boundaries worth stating

**`--accel` and `--no-accel` are `build`, `run` and `test` options** (run and
test from 2026.9.5.2), as `--target` and `--profile` are. `pack` reads
`[build] accel` from the manifest like every other build input. The flag was a
`build`-only option at first, and the measured consequence was a project whose
CPU-only variant could be built and not run: `mcpp build --no-accel` produced
it, and `mcpp run` handed back the device build.

**`mcpp pack` does not emit the `accel` field.** It could write whatever the
manifest declared, and that is exactly why it does not: the field states what an
artifact *carries*, and mcpp does not yet compile device code itself — form A
goes through a build-rule package, so mcpp has nothing to measure. Recording a
declaration in a field whose meaning is "measured" would make the identity lie
in precisely the way the dimension exists to prevent. A publisher writes the
field explicitly today, which is what index descriptors do; `mcpp pack` will
emit it once `kind = "device"` puts the device compilation inside mcpp.

## Not implemented

The whole-target shape (SYCL, OpenMP offload, stdpar), device targets and the
device linking they imply, static libraries containing device code, and
accelerator payloads supplied through xim. See
`.agents/docs/2026-09-05-accelerator-support-design.md` for the design these
follow from.
