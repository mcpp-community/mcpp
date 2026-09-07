# 20 — Heterogeneous Builds

GPU and AI accelerator targets, and mixed host/device compilation: how mcpp
builds device code, and how a prebuilt artifact states which devices it can
run on.

## What is supported

A build names the device backends it targets, and it may name **several**:

```toml
[build]
accel = "cuda12.9+{sm_89}, vulkan1.2"
```

`accelerator` is a target-side **set**, so both `cfg(accelerator = "cuda")` and
`cfg(accelerator = "vulkan")` are true in that build, each backend's rule
package compiles its own device units, and one artifact carries all of them.
The comma separates entries in that set; a space separates modifiers WITHIN one
entry (`cuda12.9+{sm_89} ptx>=89` is one backend with an architecture set and a
PTX floor). See "Several backends in one build" below for how the source sets
are then written.

Four programming models have rule packages today -- CUDA, HIP, SYCL and
Vulkan/SPIR-V -- and the table under "The lanes" says which compiler and which
payloads each drives. Nothing in the engine holds a vendor name, so a fifth is
a package rather than an engine change.

## Two shapes, and why one mechanism reaches both

Accelerator toolchains come in two shapes. They describe how a toolchain is
normally used, not how many mechanisms a build system needs.

An **island** keeps device code in separate translation units compiled by a
separate compiler. CUDA, HIP, Ascend C and Metal all work this way. The device
compiler produces an object (or, for shading languages, a runtime resource)
that joins the ordinary link.

A **whole-target** model puts device code in ordinary `.cpp` files and compiles
the entire target with a compiler capable of offloading. SYCL, OpenMP offload
and stdpar are used this way.

mcpp implements ONE mechanism, the island. That is a statement about the
DESIGN, not about how many backends are supported: a whole-target toolchain is
reached **through** the island rather than beside it, so supporting it costs a
rule package and no second mechanism. SYCL is in the shipped set for exactly
this reason.

**SYCL's can.** A SYCL kernel is a lambda inside a `submit`, so a project can
confine every one of them to a translation unit of its own by convention, and
`.sycl` is that convention made checkable. Only those units go to the SYCL
compiler; the rest of the target is compiled by the project's own toolchain,
which is what `examples/09-heterogeneous/sycl` shows -- `app.cppm` and `main.cpp` by
mcpp's clang, `saxpy.sycl` by the dpcpp payload's.

**OpenMP offload's and stdpar's cannot.** `#pragma omp target` and
`std::execution::par_unseq` appear at arbitrary call sites in ordinary code;
there is no unit to move, so there is no island to impose. Those two are what
"the shape mcpp does not implement" now means, and the boundary is a property
of the model rather than a gap in this document's ambition.

Two consequences worth stating plainly:

* An existing SYCL project whose kernels sit in `.cpp` files does not build
  unchanged. Its device units move into `.sycl` first, which is a rename and a
  seam, not a rewrite.
* One extension means one thing. Letting a constrained glob carry `.cpp` would
  make the same name select two different compilers depending on which glob
  matched first, and the seam is legible precisely because the file name says
  which side of it a unit is on.

## Device translation units

A source in a language a separate compiler consumes is a **device translation
unit**. mcpp classifies it as such and treats it accordingly: it is never
scanned for imports and never produces a BMI, because no device compiler
accepts C++20 modules.

The criterion is the compiler, not the language (the table below is
2026.9.5.3+; before it, `.cu` and `.hip` alone). `.sycl` is where that
distinction becomes visible: its content is ordinary C++ and nothing in the
file would tell a reader otherwise. What makes it a device unit is that it goes
to a compiler with a device back end -- one mcpp does not drive, and one that
does not accept C++20 modules.

| language | extensions |
|---|---|
| CUDA, HIP | `.cu`, `.hip` |
| SYCL | `.sycl` (2026.9.6.1+) |
| Ascend C | `.asc`, `.cce` (2026.9.6.5+) |
| GLSL, by stage | `.comp`, `.vert`, `.frag`, `.geom`, `.tesc`, `.tese`, `.mesh`, `.task`, `.rgen`, `.rint`, `.rahit`, `.rchit`, `.rmiss`, `.rcall` |
| GLSL, stage-less | `.glsl` |
| HLSL | `.hlsl` |
| OpenCL C | `.cl` |
| Metal Shading Language | `.metal` |

An extension outside this table listed in `[build] sources` is refused by name,
which is the behaviour that makes the table a table: mcpp has no rule for the
file, its object would be linked by nothing, and building it would fail later
and less clearly.

`.glsl` carries no stage. glslang derives the stage from the extension, so a
rule package refuses a stage-less name — the message belongs there, and this
table therefore does not need to know which extensions name a stage.

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
`examples/09-heterogeneous/cuda` for a working CUDA rule.

The division is deliberate. mcpp owns the graph, the artifact's identity and
the set of architectures; a vendor's flag spelling, its architecture syntax and
its host-compiler requirements belong to the rule.

## What the rule package reports before the first compile

Three things go wrong late with a device toolkit, and none of them is a fact
about the build graph. They are read and reported by the **rule package** that
drives the tools -- `mcpp.rules.cuda` in `mcpp:plugins` shows each one -- and
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

## Several backends in one build

`accel` names a set, so a project supporting more than one device writes one
section per backend and the sets compose:

```toml
[package]
accelerators = ["cuda", "vulkan"]

[build]
accel   = "cuda12.9+{sm_89}, vulkan1.2"
sources = [
  "src/*.cppm", "src/*.cpp",
  { glob = "src/kernels/**/*.cu",  accel = "cuda12.9+{sm_89}" },
  { glob = "shaders/*.comp",       accel = "vulkan1.2" },
]

[target.'cfg(accelerator = "cuda")'.build]
sources = ["src/cuda/*.cpp"]

[target.'cfg(accelerator = "vulkan")'.build]
sources = ["src/vulkan/*.cpp"]
```

Each constrained glob is narrowed by the set, so `--accel vulkan1.2` alone
compiles the shaders and leaves the `.cu` out without any hand-written
condition.

### The CPU fallback, when there is more than one backend

The four examples each write `cfg(not(accelerator = "<its own>"))`, which is
correct for a project with ONE backend and wrong for a project with several: a
CUDA build satisfies `not(accelerator = "vulkan")` too, so the CPU
implementation joins the link beside the CUDA one. With a seam they define the
same `extern "C"` symbols, and the result is measured:

```
ld: obj/src/cpu/impl.o: multiple definition of `impl';
    obj/src/cuda/impl.o: first defined here
```

Loud, and at the link rather than at run time -- but it is a failure the
manifest can avoid. Negate the whole set:

```toml
[target.'cfg(not(any(accelerator = "cuda", accelerator = "vulkan")))'.build]
sources = ["src/cpu/*.cpp"]
```

`any`, `all` and `not` compose over the membership test as ordinary boolean
combinators. The cost is that the list is a second place the backend set is
written: a third backend added without extending this line produces the link
error above.

### The other shape: no link-time selection

The exclusion exists only because a **seam** replaces one implementation with
another, so exactly one may be linked. A project that wants one binary to run
on whatever the machine has should not select at link time:

* compile **every** backend it was built with -- each `cfg(accelerator = ...)`
  section adds its own sources, and the CPU implementation is unconditional;
* give each one a distinct name, and have the seam ask at run time which
  devices are present.

There is then no `not(any(...))` to maintain, and the artifact answers the
question the user of the binary actually has. This is the shape ggml uses: each
backend registers itself, and `ggml_backend_reg_by_name` picks one when the
program runs. `ggml-org:llamacpp` is built that way, and its `backend-vulkan`
feature is additive over `backend-cpu` rather than exclusive with it.

Which shape to choose is a property of the program, not of mcpp: a seam that
swaps an implementation wants link-time selection, and a program that ships to
machines it has not seen wants run-time selection.

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

## The lanes, and what each one drives

A rule package owns one compiler's spelling. The engine knows none of these
names: `tests/unit/test_core_vendor_probes.cpp` asserts that no vendor tool
name appears in `src/` once comments are stripped, with the file count as its
own denominator.

| feature of `mcpp:plugins` | module | compiler it drives | payloads it needs | `[build] accel` |
|---|---|---|---|---|
| `rules-cuda` | `mcpp.rules.cuda` | the project's own clang (`-x cuda`), or nvcc with a GCC toolchain | `xim:cuda-nvcc`, `xim:cuda-cudart`, `xim:libcurand`, `xim:cuda-cccl` | `cuda12.9+{sm_89} ptx>=89` |
| `rules-hip` | `mcpp.rules.hip` | the project's own clang (`-x cuda`) on the NVIDIA platform | the above plus `xim:hip-nvidia` | `hip, cuda12.9+{sm_89}` |
| `rules-sycl` | `mcpp.rules.sycl` | the `xim:dpcpp` payload's clang (`-fsycl`) | `xim:dpcpp`, `xim:gcc`, `xim:cuda-nvcc` for an NVIDIA target | `sycl` or `sycl, cuda12.9+{sm_89}` |
| `rules-spirv` | `mcpp.rules.spirv` | `glslangValidator` or `glslc` | `xim:glslang` or `xim:shaderc` | `vulkan1.2` |

Two chunks, not one, in the `accel` value of the HIP and SYCL rows. The first
names the programming model and the second names the device, so a device is
spelled once in this ecosystem however many models reach it: `sm_89` is the
same `sm_89` whichever rule reads it. `accel = "sycl"` with no second chunk
compiles to SPIR-V and lets the runtime choose a device.

**HIP on the NVIDIA platform is a header layer, not a second runtime.** Every
HIP entry point is an inline wrapper over the CUDA one, so the object links
against the CUDA runtime and there is no ROCm on the machine. `xim:hip-nvidia`
therefore contains no binaries. The AMD platform needs a ROCm runtime this
ecosystem does not publish yet, and the rule refuses it by name rather than
producing an object nothing can link.

**A SYCL build carries two C++ runtimes, and the seam is what makes that safe.**
`libsycl.so` is compiled against libstdc++ while an mcpp artifact links libc++,
so both are in the image and mcpp's duplicate-symbol check reports the
unwinder symbols they share. Nothing may cross the seam: a SYCL exception is
caught in the device translation unit and returned as a code, because the
runtime that threw it is not the one the caller would unwind with.

## What a framework looks like on top of this

The four lanes prove a rule package can drive four compilers. A framework is
the next question -- whether the mechanism carries something a person would
deploy -- and the answer has a shape of its own, measured on llama.cpp's Vulkan
backend.

**A framework brings its own generator, and the ecosystem should drive it
rather than replace it.** llama.cpp produces its shaders with a tool it keeps
beside the backend that consumes them; the two are one contract in one
repository. A rule package that reimplemented the generation would be a second
version of that contract, drifting on its own schedule. `mcpp.rules.spirv`
exists for a project that writes shaders; a project that already has a shader
pipeline needs its pipeline DECLARED, not replaced.

**Declaring is the whole difference at that scale.** 134 shader sets generated
inside a build program are serial, run once per prepare, and report a failure
as `build.mcpp exited 1`. The same 134 as `mcpp::action` edges are incremental,
parallel, and each names itself when it fails. The threshold where this stops
being a preference is low: `mcpp.rules.sycl` already declares two.

**A capability probe belongs in the build program, and only once.** Which
extensions a shader compiler accepts is a property of how it was built, not of
its version, so upstream reads the compiler's own refusal. That answer has two
readers -- the generator, which decides which variants to emit, and the backend
source, which decides which to look for -- and both are fixed before anything
compiles. Asking twice would make one truth into two.

**The optional backend is a feature, and everything it needs hangs off that
feature.** `[feature-deps.<f>]` for packages and `[feature-xlings.<f>]` for
tools, so a consumer that does not name the backend acquires none of it. The
criterion for that claim is not that the CPU build still works; it is that the
CPU build's resolution names no package belonging to the backend.

**A software device is not automatically a substitute for hardware.** ggml
keeps only Vulkan devices whose type is not `eCpu`, so Mesa's lavapipe is
excluded for its type alone even though it advertises every feature the backend
requires. That is the framework's policy, not a packaging defect, and the way
past it is the framework's own selector rather than a change to the packaging.

`ggml-org:llamacpp` carries this as its `backend-vulkan` feature.

## Not implemented

Device targets and the device linking they imply for the island shape, OpenMP
offload and stdpar, the AMD platform of HIP, and Metal. See
`.agents/docs/2026-09-05-heterogeneous-build-ecosystem-design-v2.md` for the
design these follow from and the reason each is open.
