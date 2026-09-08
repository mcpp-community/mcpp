# A module-first surface for graphics acceleration and heterogeneous computing

Date: 2026-09-07. Base: mcpp `81358046`, `mcpp-plugins` `e75e4f5` (0.2.5).

**How to read this document.** It is four documents under one heading, and
which one a section belongs to decides whether it describes the system as it is
now:

| Sections | What they are | Current? |
|---|---|---|
| 1 to 12 | The design, argued against the base commits above | A SNAPSHOT of 2026-09-07 |
| 13 | The plan, and what carrying it out found | Historical record |
| 14 | What is open after the release, and each one's fix | Current |

Sections 1 to 12 were not rewritten as the work landed. Fourteen claims in them
no longer hold, in three different ways: a syntax that was designed and
withdrawn (`[rules]`, in 5.2, 5.5, 6 and 8.D), a mechanism that shipped in a
different shape from the one argued for (4.1's host-tool binary), and items
listed as open that have since closed (1, 3.5, 7, 12). Rather than delete
them -- an argument's rejection is part of its record, and so is the shape a
design took on contact -- each carries an in-place note naming what shipped
instead and the section that says why.

Section 14 is the only section to read for "what is open"; section 1's own
"Still open" list is a snapshot and is annotated as one. Where a status is
stated, it is stated ONCE: "which rules pass a depfile" lives in 14.2 and
nowhere else, because three copies of it were three places to forget when it
changed.

mcpp 2026.9.7.1 and `mcpp:plugins` 0.3.0 are the releases sections 13 and 14
were written against; 14.1 and 14.2 are closed by mcpp 2026.9.8.1 and
`mcpp:plugins` 0.4.0, and say so in place.

Scope: the consumer-facing surface of every heterogeneous lane -- CUDA, HIP,
SYCL, Ascend C and SPIR-V -- and the embedding of device payloads into the
artifact. The thesis is one sentence: **a generated header should be an
intermediate that no consumer names, and the only surface a consumer sees
should be a C++ module.** Everything below is either an argument for that
sentence or a consequence of it.

All measurements were taken on this machine (g++ 16.1.0, binutils 2.42,
32 cores) and the method is stated so each can be reproduced or refuted.

## 1. What is already true

This section records the state as of the base commits, not as of an earlier
reading. `mcpp-plugins` 0.2.5 closed several items that an earlier draft of this
analysis listed as open.

The engine layer carries no vendor knowledge and is one system. The accelerator
axis is a string the engine compares as a shape and places in the fingerprint;
the constrained glob routes device sources; `mcpp::action` with its four roles
is the only graph primitive; `mcpp::fact`, `mcpp::floor` and `mcpp::warning` are
the only channels back; payloads are declared under `feature-xlings` behind two
gates; and a device source no rule claimed is refused at the one place that can
see every rule's share.

Five rules share one skeleton: read their chunk of `mcpp::accel()`, claim their
extensions, discover payloads through `xpkg_dir`, expose `plan`/`submit`, and
refuse before compiling rather than after.

Closed in 0.2.5, and not to be re-proposed:

- `popen` is guarded and spelled `_popen` on Windows (`rules/spirv.cppm:276`,
  `rules/sycl.cppm:222`).
- `PATH` is split on the platform's separator (`kPathSep`, `rules/spirv.cppm:203`).
- CI has a `rules-cross-platform` matrix over macos-15 and windows-2022 running
  "every rule module compiles for this host", alongside the Linux consumer job.
- Windows shapes exist for CUDA and SYCL. `_WIN32` guard counts are now
  cuda 6, spirv 4, sycl 3, hip 1, ascendc 0 -- the last being legitimate, since
  Ascend has no Windows platform.
- Two shaders whose stems collide are refused naming both files, the symbol and
  the way out, with a CI step asserting each of those five strings.

Still open **as of the base commits**, and load-bearing for what follows. Two of
the three have since shipped; the list is kept because sections 3 to 8 argue
from it, and each entry says where it stands now. Section 14 is the register of
what is open today.

- `mcpp::action` has no depfile field. Its fields are `id`, `role`,
  `description`, `blocking`, and the `input`/`output`/`arg`/`provides`/
  `imports`/`target` setters. Inputs are fixed at submission.
  **Superseded:** `mcpp::action::depfile` shipped in mcpp 2026.9.7.1. Whether
  any rule passes one is 14.2, which is the only place that says.
- `glslc` cannot be selected by declaration; `find_compiler` consults
  `xpkg_dir("glslang")` before `xpkg_dir("shaderc")` and the rule declares
  glslang unconditionally, so glslang wins in every build that works.
  **Still open**, unchanged.
- Nothing generates a module. `mcpp.tools.embed` and `mcpp.rules.spirv` both
  emit headers that the consumer includes by name.
  **Superseded:** `mcpp.plugins.surface` shipped in `mcpp:plugins` 0.3.0 and
  every rule emits a module by default. The header remains as the surface a
  project with `modules = false` gets, which is 5.4.

## 2. Two kinds of lane, and only one of them wants a generated surface

The five rules are one system, but their payloads are not one kind of thing, and
the difference decides who writes the interface.

**Code lanes: CUDA, HIP, SYCL, Ascend C.** The device translation unit is code.
Its interface is a design decision -- which functions, which types, what happens
on failure -- and no generator can make that decision well. The example lanes
already carry the right shape: `src/kernels/saxpy.cu` is an island with an
`extern "C"` header, and `src/app.cppm` is a hand-written seam that turns raw
pointers back into a C++ interface. The `extern "C"` header is required, because
the island is compiled by a host compiler mcpp did not resolve and the two sides
share no C++ ABI.

**Data lanes: SPIR-V, and `tools.embed` generally.** The payload is data. Its
interface is mechanical: an address and a size. There is nothing to design, and
therefore nothing a generator can get wrong.

This distinction has a consequence worth stating plainly, because it is the
whole design:

> The CUDA lane already does what this document asks for. `saxpy/saxpy.h` is a C
> header that only the seam includes; every consumer writes `import app.saxpy`
> and never names the header. The header is an intermediate. The SPIR-V lane
> does not, because there the generated header *is* the surface.

So the work is not to invent a shape. It is to give the data lanes the shape the
code lanes already have, and to generate it rather than ask each project to
write it.

## 3. Measurements that constrain the design

### 3.1 Where the bytes should live

Method: 100 payloads of 4096 32-bit words (16 KB each, the size of an ordinary
compute shader), 1.6 MB total. Header route: one `const uint32_t[]` header per
payload, ten per translation unit, eleven TUs. Object route: 100 binaries
through `objcopy -I binary`, then link. Both `-O2`, both `-P 32`.

```
header route    compile 0.64s + link 0.44s                = 1.10s
object route    objcopy 1.28s + compile 0.44s + link 0.46s = 2.17s
```

The header route is twice as fast, which contradicts the intuition that parsing
integer literals dominates. Marginal cost per route, with each program's own
startup subtracted (`g++` on an empty TU: 0.46s; `objcopy --version`: 0.204s --
both unusually large here because payloads load through the subos farm):

| Array | Bytes | `g++` marginal | `objcopy` marginal |
|---|---|---|---|
| 4 K words | 16 KB | ~0 | ~0.006s |
| 16 K | 64 KB | 0.02s | ~0.006s |
| 64 K | 256 KB | 0.04s | 0.016s |
| 256 K | 1 MB | 0.31s | ~0 |
| 1 M | 4 MB | 2.31s | 0.116s |

A real shader is 2 to 50 KB, the leftmost cell, where neither route has a
measurable marginal cost and the total is decided by process count -- which
favours the header route, because one compiler invocation absorbs many headers.

The crossover is the **total embedded byte count**, not the payload count.
Below roughly 1 MB the header route wins; above roughly 4 MB the compiler's
slightly superlinear curve loses by an order of magnitude. Source expansion is a
constant 2.75x (1.6 MB of binary became 4.4 MB of C source).

The current default is therefore correct. The reason the rule gives for it --
that SPIR-V is data rather than code -- does not predict this curve and would
have chosen the same route at any size.

### 3.2 What may go into a module interface

Method: 1 MB of data, reached four ways, each compiled with `-fmodules -O2`.
Consumers take the address so the data is odr-used and cannot be folded.

| Shape | BMI or source | Consumer |
|---|---|---|
| Data in the module (`export inline constexpr`) | BMI 6 282 240 B | 0.54s |
| Data in a header | source 3 145 728 B | 0.68s |
| Data in an object, interface returns `std::span<const std::uint32_t>` | **BMI 1 313 968 B** | 0.45s |
| Data in an object, interface returns a std-free POD | **BMI 1 808 B** | 0.49s |

Two findings, and the second is the larger.

**Data must not go into a module.** The BMI is six times the data and the
consumer saves twenty per cent, because GCC still deserialises the array rather
than reading it as bytes. Six times the size, carried by every consumer and
invalidated by every compiler version change, does not buy twenty per cent.

**A std type in a module interface costs 727x the BMI of a std-free one**, and
that cost is fixed rather than proportional to the data: the 1.28 MB is
`#include <span>`'s templates, present whether the payload is 16 KB or 16 MB.
This restates a conclusion this project has already paid for -- a new module
interface carrying std types poisons every downstream BMI, and the error surfaces
on an unrelated module. The generated interface must be std-free.

There is a second, independent reason for the same decision. A first attempt at
this measurement failed outright: `import std;` requires `std.gcm` to have been
built, and a project with `modules = true, import_std = false` has not built it.
A std-free interface needs neither.

### 3.3 An incidental hazard worth documenting in the tool

An `inline constexpr` array that is only partially used is folded and never
emitted. A first measurement of the module route reported a 1 KB object for 1 MB
of data for exactly this reason. SPIR-V is unaffected because the whole array is
odr-used, but a consumer of `tools.embed` that reads one element and expects the
bytes to be in the binary will not find them there.

### 3.4 Generated module interfaces are already first-class

`mcpp::action::provides()` and `imports()` exist, and `providesModule` is read
throughout `src/build/ninja_backend.cppm` -- the split-BMI path (1603, 1723), the
`scan_overrides` path (1760), and `--expect-provides` (1777).
`docs/07-build-mcpp.md:517` shows the call. A generated `.cppm` that declares
what it provides is an ordinary node in the module graph today.

Nothing is missing. The mechanism exists and no rule uses it for a payload.

### 3.5 Both shader compilers already emit dependencies

Verified against the installed payloads:

```
glslangValidator  --depfile <file>     "writes depfile for build systems"
glslc             -MD -MF <file>       "Generate make dependencies and compile"
slangc            -depfile <file>
```

`mcpp::action` has no field to receive one.

> **Superseded on both halves.** `mcpp::action::depfile` shipped in mcpp
> 2026.9.7.1 and every rule passes one from `mcpp:plugins` 0.4.0. The table
> above was read from each tool's help text; each was later run, and what it
> writes is what decides whether the flag is worth passing:
>
> ```
> glslangValidator --depfile   ->  out.spv: scale.comp ./common.glsl
> glslc -MD -MF                ->  out2.spv: scale.comp common.glsl
> slangc -depfile              ->  out.spv: <entry>.slang <included>.slang
> nvcc -MMD -MF                ->  n.o : k.cu \ common.cuh
> clang++ -MMD -MF             ->  c.o: k.cpp common.cuh
> bisheng -MMD -MF             ->  k2.o: k.asc inc.h
> ```
>
> The last three are the code lanes, which this section did not cover.
> `-MMD` rather than `-MD` there: `-MD` on BiSheng was measured pulling in fifty
> host headers under `/usr/include`, which makes an object depend on absolute
> host paths a shared build directory must not carry.

## 4. Architecture: three planes

The design is to separate three questions that are presently one.

**The producer plane** decides which compiler turns a device source into bytes
or an object. This is `mcpp.rules.*` and it is already unified.

**The storage plane** decides where those bytes live: generated C source, an
object section, or a file beside the artifact.

**The surface plane** decides how a consumer names them: a C++ module, or a C
header.

Today the three are entangled in each rule. `rules.spirv` asks the shader
compiler to emit a C declaration, so the producer decides the storage; the two
shader compilers disagree about what that means -- glslang's `-x --vn` writes a
complete declaration, glslc's `-mfmt=c` writes a bare initialiser list -- so the
rule carries a branch for each and writes one of the two headers itself in
`wrap_glslc_output`. Roughly half the branching in that file exists to reconcile
one disagreement about a question the producer should not be answering.

After separation:

```
device source ──producer──> bare payload ──storage──> .S | .h | sidecar
                                              │
                                          surface ──> .cppm  (default)
                                                      .h     (modules = false)
```

The producer emits a bare `.spv` and stops. `--vn` and `-mfmt=c` are no longer
passed, and the flavour branch disappears from everything except flag spelling.

### 4.1 Why the storage plane must be a tool, and why that tool needs a binary

`mcpp.tools.embed` is already the storage plane for one case. Its
`element::word32` exists specifically for SPIR-V, and its header says so. No
rule can call it, and the obstacle is altitude rather than oversight: the tool
reads bytes while `build.mcpp` runs, and a `.spv` the graph has not produced yet
does not exist at that moment.

The change is to give the tool a second incarnation -- a host tool binary the
graph invokes as an action. mcpp already has the mechanism: a dependency that
produces a host tool, built through a nested host sub-build into a tool store
(issue #355). The library form stays for data files checked into a project,
where reading at build-program time is correct and cheaper.

What this buys, in order of value:

- The two shader flavours produce identical output. The rule keeps only the
  knowledge that justifies its existence: which compiler, which flags.
- Storage and surface become orthogonal options rather than three interfaces.
- Any future producer of SPIR-V -- `slangc`, `dxc` -- joins at no cost, because
  embedding is no longer the producer's business.
- The same path serves CUDA cubins and fatbins, textures, fonts, model weights
  and ICD JSON. The tool stops being shader-specific.

> **Deferred, then built -- and the interval between is 14.1.**
>
> 0.3.0 shipped the library form only: `mcpp::plugins::surface`, a generator in
> the collection's lib root that every rule calls FROM `build.mcpp`, writing the
> interface, the implementation and the `.S` at plan time. All four values above
> were obtained that way -- both shader flavours produce identical output,
> storage and surface are orthogonal options, `mcpp.rules.slang` joined at no
> cost, and nothing in the generator is shader-specific -- so the binary looked
> like a refinement that could wait.
>
> It was not a refinement. The one property only the binary gives is the one
> this section names in its title: a tool the GRAPH invokes has its inputs in
> the graph. A generator that runs at plan time writes a `.S` naming a payload
> that does not exist yet, and NO channel added afterwards can make that file an
> edge. That is 14.1, and it was live in the published ecosystem for one
> release.
>
> `mcpp:plugins` 0.4.0 builds it. It is reached through `tools = [...]` on the
> dependency edge -- built from the package rather than published separately,
> for the version-skew reason docs/05 section 2.14 gives -- and only object
> storage needs it, so the default path still runs no program.

### 4.2 One tool with options, not three

`bin2c`, `bin2cppm` and `bin2obj` as three names would force a consumer to
change its import in order to change storage, which is the cliff
`docs/07-build-mcpp.md` forbids. Identifier derivation, namespace handling, the
size symbol, and the content comparison that avoids spurious rebuilds are common
to all three.

```cpp
export namespace mcpp::tools::embed {
    enum class storage { header, object, sidecar };
    enum class surface { module_, c_header };
    enum class element { byte_, word32 };
}
```

`sidecar` earns its place -- shader hot reload, weights too large to link,
runtimes that require a path -- and it is the only one that needs engine work:
`role = "artifact"` plus an entry in the `runtime.artifacts` allow-list that
`mcpp pack` consults.

### 4.3 The object storage should use `.incbin`

```asm
    .section .rodata
    .globl blur_comp_spv
    .balign 4
blur_comp_spv:
    .incbin "…/blur.spv"
    .globl blur_comp_spv_end
blur_comp_spv_end:
```

gas and clang's integrated assembler both accept this on every platform mcpp
resolves a toolchain for, so the tool needs no knowledge of ELF, Mach-O or COFF,
and there is no `objcopy` process per payload -- section 3.1 measured 100
parallel invocations at 1.28 seconds, essentially all startup. The `.balign 4`
supplies the alignment `VkShaderModuleCreateInfo::pCode` requires and that a
byte-typed symbol does not carry.

The assembler resolves `.incbin` at assembly time and reports no dependency, so
the payload must be declared as an action input explicitly. This is the same
requirement as the depfile item in section 7 and shares its fix.

> **The requirement was right and the last five words were wrong, and the two
> failures are connected.** Attaching this to the depfile item's fix is why the
> implementation shipped without it: the depfile field landed in 2026.9.7.1 and
> `.incbin` was taken along with it, when in fact the depfile does not reach
> this case at all. 14.1 is the measurement and the correction.
>
> The general shape is worth naming, because it is not specific to assemblers:
> **a requirement folded into another item's fix disappears when that item
> ships.** The requirement was stated, in this document, before any code was
> written, and no test caught its absence -- because the test for the depfile
> item passed.

## 5. The module surface

### 5.1 The interface is std-free, and it is a function

A variable cannot hold one shape across the three storages, because `constexpr`
and `extern` are mutually exclusive: under object storage the bytes live in a
section and the compiler does not know their values. A function can.

```cpp
export module myapp.shaders;

export namespace myapp::shaders {

// std-free by construction: see 3.2. A consumer that wants a span writes one,
// and `<span>` is then included by the consumer rather than by this interface.
struct spirv {
    const unsigned* code;        // VkShaderModuleCreateInfo::pCode
    unsigned long   size_bytes;  // VkShaderModuleCreateInfo::codeSize
};

spirv blur_comp();
spirv tonemap_frag();

}
```

The field names are taken from the consumer's API so that no arithmetic appears
at the call site:

```cpp
import myapp.shaders;

const auto s = myapp::shaders::blur_comp();
VkShaderModuleCreateInfo ci{ .codeSize = s.size_bytes, .pCode = s.code };
```

Under header storage the function returns a span over an array defined in the
same translation unit and is inlined away. Under object storage it returns
`{start, end - start}` over two extern symbols. Under sidecar storage it reads
or maps the file. The signature does not change, so storage becomes an option a
project revises without touching a consumer.

### 5.2 Module naming

`mcpp.*` is reserved for rules the project maintains, and the engine warns on it
keyed by package namespace. A generated module therefore lives under the
consuming package's own name.

```
default   <package name, sanitised>.shaders        package myapp -> myapp.shaders
explicit  [rules] spirv = { module = "myapp.gfx.shaders" }
```

> **Superseded.** The `[rules]` manifest table was designed here and withdrawn
> before implementation; 13.1.2 gives the argument. `grep '"rules"'
> modules/manifest/src/toml.cppm` returns nothing, and it is not planned. What
> shipped is a field on the rule's own options, set in `build.mcpp`:
>
> ```cpp
> mcpp::rules::spirv::options o;
> o.module_name = "myapp.gfx.shaders";   // empty derives the default
> mcpp::rules::spirv::compile(o);
> ```
>
> The DEFAULT is as written above and is the part that mattered. Its derivation
> was implemented from the wrong question -- the package DIRECTORY's name rather
> than the package's -- and 13.3.1 records the correction; the rule as stated
> here always held.

**One module per package, not one per payload.** 134 shaders as 134 modules
would be 134 BMI files, 134 graph nodes and 134 import lines. One module is a
single BMI of roughly 2 KB holding 134 declarations.

`myapp.shaders` is a cohesive name rather than a grab-bag: one kind of thing,
one producer, one reason to change. The prohibition on pocket modules is not in
tension with it.

### 5.3 Namespace naming

**The module name and the namespace are the same identifier path, with `.` and
`::` exchanged.** Subdirectories add a namespace segment and no module.

```
myapp.shaders                 ->  namespace myapp::shaders
shaders/blur.comp             ->  myapp::shaders::blur_comp()
shaders/post/tonemap.frag     ->  myapp::shaders::post::tonemap_frag()
```

Nothing has to be looked up. This also dissolves the stem collision that 0.2.5
resolved by refusal: `a/scale.comp` and `b/scale.comp` become
`myapp::shaders::a::scale_comp` and `myapp::shaders::b::scale_comp`, and the
refusal remains only for a genuine collision within one directory.

The identifier keeps the stage -- `blur_comp` rather than `blur` -- because
`blur.comp` and `blur.frag` would otherwise collide, and drops the `_spv`
suffix, which the namespace already states. The stage is kept uniformly rather
than only when needed; conditional naming is worse than verbose naming.

The project's own example writes `export module app.saxpy;` and opens
`namespace app`. A hand-written module's author may choose. Generated code has
no reason to surprise anyone.

### 5.4 The header becomes an intermediate

Under the module surface, the generated header (or the generated `.S`) is
consumed only by the generated implementation translation unit. No consumer
names it, no consumer's `#include` mentions it, and `mcpp::include_dir` does not
need to advertise it on the consumer's include path.

Under `surface = c_header` -- a project with `modules = false` -- the same
header becomes the surface. There is one artefact, and which role it plays is
decided by one option.

### 5.5 The default follows the project, not a preference

| `[language] modules` | default surface |
|---|---|
| `true` | module |
| `false` | C header |

The user makes no new decision, and the default cannot be wrong.

> **Superseded in its mechanism, not its rule.** The table above is what
> shipped: `mcpp::plugins::surface::default_surface()` reads
> `MCPP_LANGUAGE_MODULES`, which mcpp sets from `[language] modules`, and an
> engine that does not report it leaves the header surface in place -- so a
> consumer of an older engine keeps the behaviour it had before the surface
> existed. The override is `options::surface`, set in `build.mcpp`, not a
> `[rules]` entry; see the note in 5.2.

### 5.6 A generated module and the `--no-accel` build

A build with no accelerator leaves the constrained glob out, so the rule sees no
device sources and generates nothing. A consumer that writes
`import myapp.shaders;` from a translation unit compiled in every build would
then fail to compile in exactly the CPU-only configuration CI runs.

The resolution is the seam pattern the code lanes already use, and it needs no
special case: the generated shader module is imported only from the
accelerator-side implementation behind the project's own seam, exactly as
`saxpy.cu` is compiled only when the build names CUDA. Under `--no-accel` that
translation unit is not compiled and nothing imports the module.

This must be stated rather than assumed, because the failure is a compile error
in a configuration the author may not build locally. The engine can see it: it
knows the module graph and which translation units are `cfg`-gated, so an import
of an accelerator-produced module from an ungated TU is a diagnosable condition
rather than a link error.

> **Not implemented.** Stage C in section 8 said to add this diagnostic "in the
> same change"; the surface shipped without it and nothing recorded the
> omission, so it was neither done nor open. It is now 14.7. The seam pattern
> above is what every converted example uses and it does hold -- what is missing
> is only the engine telling an author who did not use it what went wrong.

## 6. Layers, and where the simplicity comes from

`docs/07-build-mcpp.md` already fixes the rule: layers must not have a cliff,
and each layer must be the composition of the one below it. `generate_all(opt)`
is `submit(plan_all(opt))`. The rules honour this internally and expose
`plan`/`submit`.

What is missing is the layer *above* `build.mcpp`. The four lines every project
writes identically, plus `host-module = true` and the feature name, are
configuration expressed as construction.

| Layer | Form | Audience |
|---|---|---|
| **L0** | `[build] accel` plus a glob; nothing else | most projects |
| **L1** | `[rules] spirv = { storage = "object", includes = ["shaders"] }` | tuning |
| **L2** | `build.mcpp` calling `compile(opt)` | today's only entry |
| **L3** | `plan()`, edit the edges, `submit()` | full control |

L2 and L3 exist. L0 and L1 do not.

> **Superseded.** L1 as spelled here was the `[rules]` table, which was
> withdrawn (13.1.2) and is not planned. Tuning is L2: the same values are
> fields on the rule's `options`, set in `build.mcpp`. So the ladder that
> exists is L0 (absent), L2, L3 -- three rungs, not four, and the gap is at the
> bottom rather than in the middle.
>
> L0 is still absent and still the largest single improvement to first use. It
> does not depend on L1 and never did: `[build] accel` plus a glob is the whole
> of it.

**The table L0 needs already exists in the engine.** `src/build/prepare.cppm`
prints it in the diagnostic for a device source no rule claimed, naming
`mcpp.rules.cuda` for `.cu` and `mcpp.rules.spirv` for shaders. Nothing reads
it. Lifting that mapping out of the string is most of L0's work, and it is the
same shape as a defect this project has recorded before: an answer that is
parsed correctly and then wired to no decision.

The target, for a project that wants a Vulkan compute shader in its binary:

```toml
[build]
accel   = "vulkan1.2"
sources = ["src/*.cpp", "shaders/**/*.comp"]
```

```cpp
import myapp.shaders;
const auto s = myapp::shaders::blur_comp();
```

No `build.mcpp`, no `[build-dependencies]`, no feature name, no
`host-module = true`, no generated header named anywhere. What happens behind
it: the extension routes to `rules.spirv`, glslang emits a bare `.spv`, the
embed tool writes a `.S` and a `.cppm`, the assembler produces the object, the
module is a 2 KB BMI, and the link collects both.

> **Where this stands.** Everything after "What happens behind it" shipped in
> 0.3.0 and is what a build does today. The four lines above it did not: a
> consumer still writes `[build-dependencies.mcpp] plugins = { features = [...],
> host-module = true }` and a `build.mcpp` that calls `compile()`. What 0.3.0
> removed from that list is the last item -- no consumer names a generated
> header, and the examples in `examples/09-heterogeneous` are the evidence.
>
> The remaining four are L0, and section 8's staging D is where it was to be
> done. D did not happen.

## 7. What remains open, and where each lands

**As of the base commits.** Three of the four have since been resolved and each
says so below; section 14 is the register of what is open today.

1. **No dependency tracking for included device sources.** `mcpp::action` has no
   depfile field, so `rules/spirv.cppm` declares only the `.comp` and
   `rules/cuda.cppm` only the `.cu`. Editing a `.glsl` or a `.cuh` rebuilds
   nothing and the build is green. All three shader compilers already emit a
   depfile (3.5). This is the only open item that produces a wrong artifact
   rather than a failure, and the `.incbin` dependency in 4.3 needs the same
   field.

   **Resolved, and the last sentence was wrong.** `mcpp::action::depfile`
   shipped in mcpp 2026.9.7.1 and all six rules pass one from `mcpp:plugins`
   0.4.0, each spelling measured against the tool rather than read from its
   help text. The `.incbin` dependency does NOT need the same field -- no
   assembler channel reaches it portably -- which is 14.1.

2. **`glslc` cannot be selected by declaration.** `find_compiler` consults
   `xpkg_dir("glslang")` before `xpkg_dir("shaderc")`, and the rule declares
   glslang unconditionally, so glslang wins in every build that works. The
   rule's own "no shader compiler found" diagnostic advises naming
   `xim:shaderc` and says it will win, which holds only when neither is
   installed. The project's CI corroborates: the glslc step downloads a tarball
   and sets `MCPP_GLSLC` rather than declaring the payload.

3. **The stem refusal becomes narrower once 5.3 lands.** It should stay for a
   genuine within-directory collision and stop firing for two directories.

   **Resolved in `mcpp:plugins` 0.3.0.** `tests/spirv-module-consumer` carries
   `shaders/a/scale.comp` and `shaders/b/scale.comp` and builds, which is the
   fixture that says so; the refusal remains for a collision within one
   directory.

4. **Slang.** `slangc` should become `mcpp.rules.slang` rather than a third
   flavour of `rules.spirv`: glslang and glslc are two drivers for one language
   compiling the same `.comp`, whereas Slang is a different language with a
   different extension and a target set that includes DXIL and Metal, for which
   the Vulkan axis `parse_target` reads has no answer. Its module dependency
   graph needs no new concept -- a shader module package is an ordinary mcpp
   package whose `include_dirs` names its `.slang` directory. `vulkan-rt` had to
   invent a scope API for this because xmake offered no package-level dependency
   to reuse; mcpp has one, and should use it rather than copy the workaround.

   **Resolved in `mcpp:plugins` 0.3.0.** `mcpp.rules.slang` exists and declares
   its own `device_extensions`, so `.slang` was REMOVED from the engine's
   built-in table and `tests/slang-consumer` still builds -- which is the
   measurement that a new device language costs no engine release (13.1.2).

## 8. Staging

Each stage is useful alone and none depends on a later one.

**A. A depfile field on `mcpp::action`.** The only open defect that yields a
wrong artifact with a green build. Every compiler involved already emits one.
Fixing it at the action rather than in one rule serves `cuda`, `spirv` and 4.3
at once. Independent of everything else here.

> **Shipped, and the last clause was false.** `mcpp::action::depfile` is in
> 2026.9.7.1; 14.2 tracks which rules pass one. It does NOT serve 4.3: the
> assembler reports nothing a depfile can carry (14.1). What serves 4.3 is
> moving the generation into the graph, which needed no new channel at all.

**B. `tools.embed` as a host tool, with `storage` and `surface`.** The
separation in section 4. `rules.spirv` stops passing `--vn` and `-mfmt=c`, emits
a bare `.spv`, and loses `wrap_glslc_output` and the branch around it.

**C. The module surface.** Sections 5.1 through 5.4, on top of B. The
measurement in 3.2 fixes the interface shape; 3.4 confirms the graph already
accepts it. Add the `--no-accel` diagnostic from 5.6 in the same change, because
it is the failure a user meets first.

**D. `[rules]`, then L0.** Section 6. The largest single improvement to first
use, and the only stage whose main cost is in the engine rather than in the
plugins.

> **Did not happen, and half of it should not.** `[rules]` was withdrawn
> (13.1.2). L0 is untouched and still worth doing; it never depended on
> `[rules]`, so what remains of this stage is "L0" alone.

**E. `mcpp.rules.slang`.** Nearly free after B, because embedding is by then not
the rule's concern.

## 9. Corrections carried forward

An earlier reading of this material concluded that the object route is faster at
scale because the compiler must parse the literals. Section 3.1 measures the
opposite at realistic shader sizes: the marginal parse cost is zero below about
64 K words and the total is decided by process count, which favours the header
route. The crossover is real but sits at roughly 1 MB of total embedded data and
is a property of the total rather than of the payload count.

The same earlier reading proposed `std::span<const std::uint32_t>` as the
generated interface type. Section 3.2 measures that at 727 times the BMI of a
std-free equivalent, and the cost is fixed rather than proportional to the
payload.

The same earlier reading listed the Windows `popen` and `PATH` defects and the
single-job CI as open. `mcpp-plugins` 0.2.5 closed all three; section 1 records
the current state and they are not restated as work.

## 10. Where this design sits against the alternatives

The comparison below was taken against `xmake` master and CMake 3.24+ on
2026-09-07, reading each system's source rather than its documentation.

### 10.1 xmake has already factored the storage plane

`xmake/rules/utils/` contains `bin2c`, `bin2obj`, `glsl2spv` and `hlsl2spv` as
separate, reusable rules, and `utils.glsl2spv` composes them through
`{bin2c = true}` or `{bin2obj = true}`. The separation of "which compiler
produces the payload" from "where the payload is stored" is therefore not a new
idea, and mcpp is behind on it rather than ahead. Section 4 should be read as
catching up on that axis and going further on the surface axis, not as inventing
the factoring.

Three properties of the xmake rule bound how far ahead it is:

- Compiler discovery is `find_tool` over `PATH`, ending in
  `assert(glslangValidator or glslc, "... not found!")`. The payload is not
  declared, its version is not pinned, and nothing records which compiler ran.
- `batchcmds:add_depfiles(sourcefile_glsl)` declares the source file alone.
  `--depfile` and `-MD` are not passed, so a GLSL `#include` is not tracked --
  the same defect section 7 records for mcpp, and one CMake's
  `add_custom_command(DEPFILE)` does not have.
- `bin2c` emits a bare initialiser list, so the consumer writes
  `static unsigned char x[] = { #include "..." };` by hand; `bin2obj` exposes
  raw `_binary_*_start` and `_binary_*_end`. Neither produces a namespace, a
  type, or a module.

### 10.2 The same task, four ways

| | Declaration | Consumer | Toolchain |
|---|---|---|---|
| CMake 3.24+ | about 18 lines of `add_custom_command` per shader set | writes the array declaration by hand | whatever the machine has |
| xmake | 2 lines (`add_rules("utils.glsl2spv", {bin2c = true})` plus `add_files`) | writes the array declaration by hand | `find_tool` over `PATH` |
| An in-tree rule (`vulkan-rt`) | 2 lines | `shader::name`, generated | `find_program` over `PATH` |
| mcpp today | 6 lines of manifest plus a `build.mcpp` | includes a generated header | declared payload, installed automatically |
| mcpp after this design | 2 lines of manifest | `import myapp.shaders` | declared payload, installed automatically |

### 10.3 What each system wins

**CMake** wins on ecosystem size, platform maturity and, on the one axis
measured here, dependency correctness: `DEPFILE` on a custom command is the only
one of the four that tracks a shader's own includes today.

**xmake** wins on brevity for a single-backend project and currently on storage
options, having all three.

**mcpp** wins on three axes the others do not address, and one of them is
structural:

1. *The toolchain is declared rather than discovered.* The payload is named by
   the rule, installed by the build, and recorded with `mcpp::fact`. Nothing in
   the other three answers "which compiler produced this artifact" without
   reproducing the build. This is a property of a build system and a package
   manager being one program, and it cannot be added to a rule in isolation.
2. *One accelerator axis across every backend.* A project with a `.cu` and a
   `.comp` states its targets once, and each rule claims its own extensions from
   one device-source list; a file no rule claimed is refused. In CMake and xmake
   the CUDA path and the shader path are unrelated mechanisms that agree only by
   convention.
3. *A module surface*, after section 5. No other system generates one.

**Selection rule.** A project with device code and C++20 modules is the case
mcpp is uniquely suited to. A project with one of the two is better served by
xmake today. A project with neither should use CMake.

## 11. Cross-platform constraints on the storage plane

`src/build/prepare.cppm:10243` refuses GAS sources under an MSVC toolchain, and
NASM sources are refused on non-x86 targets. The `.incbin` object storage in
section 4.3 is therefore available exactly where a GAS-capable assembler is:
every gcc and clang toolchain on Linux, macOS and Windows, and not under MSVC.

This is a constraint to state, not to work around:

> Object storage requires a GAS-capable assembler. Under an MSVC toolchain the
> embed tool falls back to header storage and says so once through
> `mcpp::warning`. The surface does not change, because the surface is a
> function; only where the bytes live does.

The fallback is safe precisely because section 5.1 made the interface a function
rather than a variable: a consumer compiled against header storage and one
compiled against object storage see the same declarations.

Sidecar storage has no assembler requirement and is available everywhere, but it
changes what `mcpp pack` must collect, which is why it is the only storage that
touches the engine.

**A second cross-platform constraint on the same storage, found later.** The
two GAS-capable assemblers do not agree on whether they REPORT what `.incbin`
reads. GNU as names the embedded file in its own `--MD` output; clang's
integrated assembler has no dependency output of any kind. So the obvious way
to keep an embedded payload current -- ask the assembler -- would work on one
toolchain and fail silently on the other, which is worse than failing on both:
the toolchain that reports nothing is the one whose users could never reproduce
the staleness. This is why the payload is a DECLARED INPUT of the action that
writes the assembly (14.1) rather than something discovered from the tool, and
it is a cross-platform argument rather than an aesthetic one.

## 12. Slang, in full

Section 7 argued that `slangc` belongs in a rule of its own. This section states
what that rule is.

> **Shipped as described, in `mcpp:plugins` 0.3.0.** Everything below holds of
> `mcpp.rules.slang` as it exists, with one addition: from 0.4.0 the rule passes
> `slangc -depfile`, so a `.slang` that includes another rebuilds when it
> changes (14.2). The version pin discussed at the end of this section is 14.6:
> resolved by fact rather than by work, because the index's ceiling and the
> package's floor are the same release.

**Extensions claimed.** `.slang`. Not the GLSL stage extensions: a `.slang` file
names its entry points internally and `-fshader-stage` has no analogue.

**Targets.** `slangc -target spirv` under a Vulkan accelerator. The rule reads
the same `mcpp::accel()` axis as `rules.spirv` and refuses when the axis names no
target it can serve. DXIL and Metal are out of scope for the first version: they
require accelerator vocabulary that does not exist yet, and adding a target the
axis cannot express would put the rule ahead of the engine.

**Payload.** `xim:slang`, declared by the rule under
`cfg(accelerator = "vulkan")` and the `rules-slang` feature, as every other rule
declares its own. A floor rather than an exact pin: the payload's version is not
coupled to a driver.

**The payload already exists, and X1 is a version bump rather than a new
package.** `openxlings/xim-pkgindex:pkgs/s/slang.lua` was merged on 2026-08-12
(PR #611) with all three platforms and both architectures, shipping `slangc`,
`slangd`, `slangi` and `slang`, and is pinned at `2026.14.1`. Upstream is at
`v2026.17` (2026-09-04). Two corrections follow from this and are carried into
section 13:

- `xim:` packages are prebuilt tool payloads and live in
  `openxlings/xim-pkgindex` (`spec = "2"`, flat names, no namespace).
  `mcpplibs/mcpp-index` (`spec = "1"`) indexes C++ source libraries. An earlier
  draft of section 13 assigned X1 to `mcpp-index`, which is the wrong repository.
- The rule can be written and tested against the published payload immediately,
  so `rules.slang` does not wait on any index change. A version bump is worth
  doing but is not a dependency.

**Module dependencies need no new concept.** A Slang module package is an
ordinary mcpp package whose `include_dirs` names its `.slang` directory; the rule
turns each resolved dependency's include directories into `-I`. This is the one
place where mcpp's package model removes work another system had to invent:
`vulkan-rt` defines a `slang(...)` scope API with `add_moduledirs` and `add_deps`
because xmake offers no package-level dependency to reuse.

**Dependency output.** `slangc -depfile <file>`, through the same `mcpp::action`
field stage A adds.

**Surface.** Identical to `rules.spirv`, because after stage B neither rule owns
the surface.

## 13. The plan

### 13.1 Work items

Status is as of this document's date. "done" means implemented and verified by a
test that was seen to fail without the change.

| Id | Repo | Item | Status |
|---|---|---|---|
| E1 | mcpp | `mcpp::action` gains `depfile`; ninja emits `depfile =` and `deps = gcc` | done |
| E7 | mcpp | `[language] modules` reported as `MCPP_LANGUAGE_MODULES` | done |
| E8 | mcpp | `[features].<f>.device_extensions` and `rule_module`: a rule package declares what it compiles and how to reach it | done |
| E9 | mcpp | `host-module = true` implied by `rule_module` | done |
| E10 | mcpp | A package with no `build.mcpp` gets the program its rules describe | done |
| E5 | mcpp | Documentation, `docs/07` and `docs/20` and their `zh` counterparts | done |
| E11 | mcpp | `tests/e2e/188`'s "did not rerun" assertion, which could not fail | done |
| E12 | mcpp | The offscreen example runs on macOS as well as Linux | done |
| P3 | mcpp-plugins | The surface: generated `.cppm` or `.h`, std-free POD interface, module and namespace naming | done |
| P4 | mcpp-plugins | `rules.spirv` and `tools.embed::group()` delegate the surface | done |
| P5 | mcpp-plugins | `rules.slang` | done |
| P1 | mcpp-plugins | `storage`: header, object through `.incbin`, sidecar; MSVC falls back to header | done |
| P8 | mcpp-plugins | Every rule feature declares `device_extensions` and `rule_module` | done |
| P7 | mcpp-plugins | Nine fixtures, one per surface, storage and language | done |
| E6 | mcpp | `.slang` in the engine's built-in table | **withdrawn** -- replaced by E8, and removed from the table to prove it |
| E2/E3 | mcpp | A `[rules]` manifest section, and activating rules from the files present | **withdrawn**, see 13.1.2 |
| P6 | mcpp-plugins | Rules pass `--depfile`, `-MD -MF` and `-depfile` | staged, needs E1 released |
| X1 | openxlings/xim-pkgindex | Raise the `xim:slang` pin toward upstream `v2026.17` | staged |
| X2 | mcpplibs/mcpp-index | `compat:vulkan` builds its Windows loader from source instead of expecting a host DLL | measured feasible, see 13.1.3; not what the Windows run step is waiting on |

### 13.1.1 A cost the implementation paid, stated rather than hidden

The generator lives in `mcpp.plugins`, the lib root, which grew from about
twenty lines to about seven hundred. The lib root is compiled for **every**
consumer of the package, including one that activates only `rules-cuda` and will
never embed anything.

It is there because a second unit beside the lib root in `[build] sources` is
not compiled as a host module ahead of the members. Measured: a member importing
`mcpp.plugins.surface` failed with `failed to read compiled module`, because
only the lib root is built first. Three members need the generator, and a
feature that two of them had to activate for the third would be a dependency
between members that the feature system does not express.

### 13.1.2 Two designs were written and withdrawn, and both for the same reason

**A `[rules]` manifest section.** `[rules] spirv = {}` would have replaced the
dependency edge and the build program. It was withdrawn because it restates what
`[build] sources` already says -- the file is there, its extension names the
rule -- and a section carrying no information the manifest does not already hold
earns its place only by expressing consent, which the dependency edge expresses
better and where the version pin belongs.

**Activating a rule from the files present.** With `device_extensions` in place,
a consumer could have named the package alone and let the extensions decide
which features turn on. Two objections, and neither was cost:

- Two packages may claim one extension. A third-party CUDA rule is a thing
  someone will write, and derivation would then guess or refuse where
  `features = ["rules-cuda"]` has already said which.
- A manifest's job is to describe the build. A derived feature set is
  information the file no longer states, which is worse for a reader and worse
  for anything reading the manifest as context.

What survived from both is the part that carries the architecture: a rule
package declares what it compiles, so the engine holds no package name, no
feature spelling and no module name -- and a new device language costs no engine
change. `.slang` was removed from the built-in table to prove that, and
`tests/slang-consumer` builds unchanged.

### 13.1.3 The Windows measurement, and the second reading that overturned it

Raising the graphics example from "builds" to "runs" on Windows was expected to
be two CI steps. It is not. The manifest declaration worked --
`Provisioning [xlings.workspace] entries (xim:mesa-lavapipe@26.2.0)` -- the ICD
manifest was found in the store, and the program printed `render unavailable`.

The first reading of that attributed it to a missing loader: `compat:vulkan`
ships an import library on Windows and nothing else, and says why in its own
descriptor -- a statically linked loader cannot work there, because upstream's
`loader_windows.c` creates its locks in `DllMain` and a static library never
gets one. The runtime `vulkan-1.dll` was said to come from an installed GPU
driver, which a runner has none of.

**That reading is wrong, and the log says so.** `render unavailable` is printed
by `src/main.cpp` after the render function returns nothing. The Vulkan leg
imports `vkCreateInstance` from `vulkan-1.dll` through the import library, so a
process that could not find that DLL would fail during image load and print
nothing at all. It printed. The loader was present, it ran, and it enumerated no
device -- which is a statement about the ICD, not about the loader. A second
fact stands with it: mcpp-index's own `vulkan-tests` member calls
`vkEnumerateInstanceVersion` on the windows shards and passes.

Two things follow.

**X2 is not what the CI step is waiting on.** A Windows loader package would
change nothing about `render unavailable`. What is unestablished is why the
lavapipe payload's ICD produces no device under a process mcpp launched, and
that is where the next measurement goes.

**X2 is nonetheless available, and it costs no redistribution decision.** The
descriptor's note argues that a Windows loader must be a DLL, not that it cannot
be built. Measured: the Khronos loader in `compat:vulkan` cross-builds into a
working `vulkan-1.dll` from the source the index already carries -- 265 exports,
matching upstream's `vulkan-1.def` name for name, all `vk*`-prefixed so
`exports = ["vk*"]` reproduces the surface exactly; `DllMain` present; importing
only ADVAPI32, CFGMGR32, KERNEL32 and msvcrt. The earlier conclusion that the
only viable source was a third-party prebuilt binary came from generalising the
descriptor's note about STATIC linkage into a claim about building at all. A
package built this way makes the loader hermetic on Windows the way it already
is on Linux and macOS, and needs no new artifact hosted anywhere.

The general form of the error is worth keeping: a recorded conclusion is
re-read, its reasoning is not. Both notes that carried it -- in
`.github/workflows/ci-windows.yml` and in the example's own manifest -- have
been corrected in place rather than deleted, so the next reader sees what was
believed and what refuted it.

### 13.2 Dependency structure across repositories

The plugins CI pins `MCPP_VERSION` to a released mcpp, so a plugins change that
uses a new engine API cannot be tested until that engine is released. This splits
the work into two waves rather than one:

```
wave 1   E1 E2 E3 E4 E5 E6 (mcpp)       P1 P2 P3 P4 P7 (plugins)
                 |
                 +--- release mcpp ---+
                                      |
wave 2                                +--> P5, P6, raise MCPP_VERSION,
                                           release plugins

any time  X1 (xim-pkgindex): raise the xim:slang pin. Nothing waits on it.
```

Wave 1's plugins work uses only engine API that exists today: the ordinary
source scan sees the generated interface and implementation, which
`mcpp::generated` adds to the build. Nothing in P1 through P4 waits on the
engine.

**P5 does, and finding out why corrected an assumption in this document.** A
constrained glob's `accel` key does not make a file a device source. The
decision is a table in the engine, `kDeviceExtensions` in
`modules/source-kind/src/source_kind.cppm`, and a file whose extension is absent
from it falls through to the ordinary source scan:

```
warning: [build] accel names vulkan but no constrained glob matched a `.slang`
error: 'scale.slang' is listed in [build] sources, and mcpp has no role for
       the extension '.slang'.
```

Measured against mcpp 2026.9.6.6 with the rule and the payload both working --
`xim:slang@2026.14.1` provisioned correctly, and the file was still refused. So
**a rule package cannot introduce a device language on its own**: the engine has
to know the extension first. That is E6, and it makes `rules.slang` a wave-2
item alongside P6 rather than a wave-1 one.

One engine addition belongs with E1 because it is what makes the plugins
default correct without coupling the two releases: mcpp reports
`[language] modules` as `MCPP_LANGUAGE_MODULES`, and the surface's default
reads it. An engine that does not set it leaves the header surface in place,
which is what every consumer had before, so an older engine keeps its behaviour
and a newer one moves a project to the module surface with nothing declared.

#### 13.2.1 The same structure, for the batch that closed 14.1 and 14.2

The constraint has not changed and neither has its consequence, so the second
batch has the same two waves and the same reason. It is recorded here rather
than left to the pull requests because the shape is now a property of the two
repositories rather than an accident of one change.

```
wave 1   mcpp 2026.9.8.1: host modules ordered by their import graph, the
         two rule-package feature keys no longer reported as unsupported,
         e2e 633, and this document's alignment with what shipped
                 |
                 +--- release mcpp ---+
                                      |
wave 2                                +--> mcpp:plugins 0.4.0: the surface made
                                           pure and split into two halves, the
                                           `mcpp-embed` tool, generation as an
                                           action under object storage, a
                                           depfile on all six rules, CI steps
                                           whose criterion is the artifact's
                                           bytes, and MCPP_VERSION raised
                                      |
wave 3                                +--> mcpp-index: 0.4.0 in the three
                                           platform tables, `latest` REPLACED
                                      |
wave 4                                +--> sandbox verification against the
                                           published packages
```

The engine half is testable on its own -- e2e 633 uses no rule package -- and
the plugins half is not, because its floor is the release that has not happened
when the pull request opens. Nothing in wave 2 can be checked by its own CI
until wave 1 is published, so a green wave-2 CI is the LAST evidence available
rather than the first, and the local run against a locally built engine is what
stands in for it.

### 13.3 Evaluation criteria, by the dimensions this work is judged on

| Dimension | What decides it | Where it is checked |
|---|---|---|
| Architecture | The surface is written once and no rule re-implements it | `rules.spirv`, `rules.slang` and `tools.embed` all call `mcpp::plugins::surface::emit`, and a fixture per member asserts the same generated shape |
| Stability | Every new field has a reader; no value is recorded and then unused | E1's depfile reaches `build.ninja`; a test asserts a rebuild after touching an included file |
| Simplicity | The smallest working project | Two manifest lines and one import, asserted by a consumer fixture |
| User experience | Nothing generated is named by a consumer | No fixture under the module surface contains the generated header's name |
| Compatibility | An existing consumer keeps building | The current spirv consumer fixture is kept unchanged and must still pass |
| Cross-platform | Every rule compiles for every host, and storage degrades rather than fails | The existing `rules-cross-platform` matrix, extended; the MSVC fallback in section 11 |
| Consistency | One naming rule, applied by both data-lane rules | `rules.spirv` and `rules.slang` produce identically shaped modules from one generator |
| Seamless upgrade | A project that writes nothing new keeps its behaviour | Header storage stays the default under `modules = false`; the default surface follows `[language] modules` |
| Test coverage | Each surface and each storage has a consumer that runs | P7 |

### 13.3.1 What the self-review found after the surface was working

Four findings, three of them in names the surface generates. Every one was found
by giving the generator an input nobody had tried, and none of them by reading
the code -- which is the reason they are recorded together.

**The user-facing name came from the wrong question.** The module a consumer
imports was derived from the leaf of `MCPP_MANIFEST_DIR` -- a directory name --
because nothing in the build-program contract answered "what package am I
building". `examples/09-heterogeneous/vulkan/app/` declares
`name = "vulkan-saxpy"` and generated `app.shaders`. Worse than the defect is
that no fixture could see it: all fourteen had a package name equal to their
directory name, so both derivations produced the same string. Fixed by
`MCPP_PKG_NAME` in the engine, and by making one fixture's two names differ on
purpose.

**A generated name may not be a C++ keyword.** Six sites ran the same character
filter and none checked for reserved words. `shaders/default/` produced
`namespace default {` in a generated file. Fixed by one `surface::identifier`
that owns all three transformations, applied at the point where the accessor is
EMITTED rather than in each producer -- a rule that builds the name from a file
stem cannot know it has produced a keyword until it reaches the line that writes
the function's name.

**A seam's two halves are compared nowhere else.** The island and the host
fallback define one `extern "C"` boundary and are never in one link, and C
language linkage does not mangle, so two that declare a name differently build
cleanly and the artifact reads its arguments by whichever it was compiled with.
`mcpp.tools.island::scan` is handed both files and is therefore the only point
at which both texts exist at once. It refuses there.

**One fix was written and withdrawn.** `host-module` is inferred from a rule
feature the consumer REQUESTS. A rule package whose rule sits in its own
`[features] default` activates without being named, so the rule modules are
collected -- that reads the resolved set -- while the inference reads the
requested one. Extending the inference to the dependency's defaults was measured
against a probe package and made the outcome WORSE: the refusal went from

    error: build.mcpp imports 'probe.rules.probe', and no dependency provides
           it as a host module.
           declared without `host-module = true`: probe.rules

to the same refusal plus `importable here: rules`, which says the dependency is
wired up while the feature's own module still is not. The gap is further down,
in when a default feature's sources are folded into the set `units()`
enumerates. `mcpp:plugins` declares `default = []`, so nothing shipped reaches
it, and the existing refusal already names exactly what to add. Recorded here
rather than half-fixed.

### 13.4 Release and verification sequence

1. mcpp pull request: E1 through E5. CI green on the head, then merged, then the
   merge commit's run confirmed green on `origin/main`.
2. mcpp release, with the GitCode assets supplied locally through `gtc`.
3. mcpp-plugins pull request: P1 through P7, `MCPP_VERSION` raised to the new
   release, package version raised.
4. Optionally, `openxlings/xim-pkgindex`: X1. Independent of the above.
5. Verification of the published artifacts in a sandbox
   (`xlings subos use N --sandbox --cmd ...`) with the CN mirror configured,
   because a sandbox is the only thing that tests what was published rather than
   what is in the working tree.

## 14. What is open after the release, and what each one's fix is

Section 7 listed what was open before the work. This lists what is open after
it. 14.1 and 14.2 are CLOSED and kept for their analysis; 14.3 through 14.5,
14.7 and 14.8 are open; 14.6 was never open.

Both of the closed entries were found by measurement while assembling the
comparison in section 10, and the first of them produced a wrong artifact
rather than a failure.

### 14.1 Object storage does not rebuild when its payload changes

**CLOSED** by `mcpp:plugins` 0.4.0 and mcpp 2026.9.8.1, and closed on the
same measurement that opened it -- in a sandbox, against the PUBLISHED packages,
resolved through the index, with the engine addressed by its store path:

```
store path: .../xim-x-mcpp/2026.9.8.1/bin/mcpp      (mcpp:plugins 0.4.0)
before: bytes=1480
after:  bytes=1776
PASS: an edited shader reaches the artifact under object storage
```

against, from the identical script and the identical edit one release earlier:

```
store path: .../xim-x-mcpp/2026.9.7.1/bin/mcpp      (mcpp:plugins 0.3.0)
before: bytes=1480
after:  bytes=1480      and `Finished dev in 0.06s` -- nothing rebuilt at all
```

The same run also confirms the refusal a project gets when it asks for this
storage and not for the tool that generates it, which is default-off:
`PASS: refused, naming the tool and the key that supplies it`. Closing on "the
pull request merged" would have been the shape of claim this entry exists to
remove.

The analysis below is kept because two of its three parts are transferable: the
requirement had been written down and lost, and the first measurement asked the
wrong tool.

Measured on `tests/spirv-object-storage` against the released 2026.9.7.1 and
`mcpp:plugins` 0.3.0, by editing the shader so its compiled output must differ
and reading the byte count the program prints:

| storage | before the edit | after |
|---|---|---|
| `header` (the default) | `bytes=1480` | `bytes=1776` |
| `object` | `bytes=1480` | **`bytes=1480`** |

And against the published ecosystem, not only against a fixture: the same edit
in an `xlings subos --sandbox`, on a project whose only reference to any of this
is `plugins = { version = "0.3.0", features = ["rules-spirv"] }`, with the engine
addressed by its store path so no development checkout can carry the result:

```
store path: .../xim-x-mcpp/2026.9.7.1/bin/mcpp
before: bytes=1480
after:  bytes=1480          and `Finished dev in 0.06s` -- nothing rebuilt at all
```

**The blast radius, stated so the severity is not overread.** `header` is the
default and `object` is what a project opts into above roughly 1 MB of total
payload (3.1). Nothing in `examples/` uses it.

#### It was written down before it was written wrong

Section 4.3 stated the requirement -- "the payload must be declared as an action
input explicitly" -- before any of this was implemented. What it added was five
more words, "and shares its fix", pointing at the depfile item in section 7. The
depfile shipped in 2026.9.7.1, the item was marked done, and `.incbin` went with
it.

> **A requirement folded into another item's fix disappears when that item
> ships** -- silently, with every test still green, because the test written for
> the other item passes.

#### The mechanism, and the comment that contains the error

`rules/spirv.cppm` handed the generated `.S` to `mcpp::generated`, so its
compile edge knew the `.S` and nothing else. The `.S` names the payload in
`.incbin` but its own text does not change when the payload does, so
`write_if_different` left the file alone, its mtime did not move, and no edge
was dirty. The shader recompiled; the object carrying its bytes did not.

The comment at that call site is worth quoting, because the reasoning in it
reads as correct:

```
// The `.S` under object storage. It is an ordinary source: mcpp assembles
// it, and `.incbin` reads the payload the action above produced, which by
// then exists because a `role = "source"` action is ordered before this
// package's compiles.
```

Every clause is true. **Ordering is not incrementality**: the payload does exist
when the object is assembled, and the object is assembled once.

#### Asking the assembler: measured on the right tool the second time

The first version of this section said a depfile cannot reach this, on this
output:

```
gcc -c p.S -MD -MF p.d    ->  p.o: p.S /usr/include/stdc-predef.h
clang -c p.S -MD -MF p.d  ->  p.o: p.S
```

That asked the PREPROCESSOR. `-MD` on a compiler driver reports `#include`, and
`.incbin` is not an include, so the answer described a channel that was never a
candidate. The assembler has its own, and the two do not agree:

```
as --MD dep.d -o p.o p.s              ->  p.o: payload.bin p.s
clang -fno-integrated-as -Wa,--MD,…   ->  out.o: payload.bin p.s
clang -Wa,-MD / -Wa,--dependency-file ->  rejected
clang -cc1as -dependency-file         ->  unknown argument
```

GNU as names the embedded file. Clang's integrated assembler does not and has no
option that would; the `-fno-integrated-as` line succeeds only by handing the
work to gas. So the assembler's own dependency output would leave `.incbin`
tracked under GCC and silently untracked under Clang -- worse than untracked
under both, because the toolchain that reports nothing is the one whose users
could never reproduce the staleness.

#### The resolution: no new engine channel, because the cause was the timing

An engine primitive was written for this and then withdrawn. It worked --
`mcpp::recompile_if_changed(source, dependency)`, protocol 9, a ninja implicit
input -- and it was unnecessary, which a prototype settled before the design was
committed to:

```
one `role = "source"` action, payloads as its declared inputs
      BYTES=64 -> BYTES=192      with no engine change at all
```

The defect was never that the engine could not express the edge. It was that the
generation ran at the wrong TIME. `surface::emit` ran at plan time and wrote a
file naming a product the graph had not made yet; a file written then cannot be
an edge to a build-time product, whatever channel is added afterwards. Move the
generation into the graph and the edge is ordinary:

```
payload changes -> the action's declared input changed -> ninja reruns it
                -> no restat, so its outputs count as new
                -> the assemble edge reruns
```

Traced file by file: `scale_comp.spv` moves, `payload.S` does NOT (its content
is unchanged, so `write_if_different` leaves it), and `payload.S.o` moves. With
the payload inputs removed, `payload.S.o` does not move and the byte count
stands still -- the control, run both ways.

#### What that cost, and why each part is right on its own terms

Three changes were needed, and none of them is scaffolding for this defect:

1. **`mcpp.plugins.surface` imports only `std`.** It used to read
   `mcpp::target_os()`, `mcpp::compiler()` and `mcpp::package_name()`; those are
   now parameters. A generator that takes its inputs rather than reading its
   environment is the same code in a build program and in a program -- and a
   program is what an action's command has to be. Measured before: an ordinary
   build of the package failed with `mcpp: failed to read compiled module`,
   because the lib root imported the build-program module.

2. **`mcpp-embed` is built from the package**, through `tools = ["mcpp-embed"]`
   on the same dependency edge that brings the rules in. Not published as a
   payload: docs/05 section 2.14 gives the reason and it is not convenience --
   "the tool's version IS the dependency's version, so a `protoc` that does not
   match its runtime is not expressible. This is the problem with packaging the
   tool separately, and it is the failure mode that bites at run time rather
   than compile time." The generator and the declarations it writes are one
   decision.

3. **A package's host modules are ordered by their import graph** (mcpp
   2026.9.8.1). They were ordered by PATH, so `rules/spirv.cppm` preceded
   `src/declare.cppm` and importing it failed. That ordering is also what 13.1.1
   recorded as the reason its lib root grew from twenty lines to seven hundred
   -- a monolith adopted because a sort was read as "a second unit is not
   compiled at all".

Two actions rather than one, and that is a fact about inputs rather than a
limitation: the interface is a function of the item list, the body of the item
list AND the payloads. One action would rewrite the interface whenever a payload
changed and rebuild every BMI importing it. (`mcpp::action::provides` is also
per-action rather than per-output, so one action with three outputs and one
`provides` makes the scanner report "already provided by" -- measured.)

Only `object` needs any of it. `header` reaches the artifact through generated
data headers the payload's own compiler already writes as action outputs, and
`sidecar` is never compiled -- checked rather than assumed. So the default path
builds no tool and a consumer that never opts into object storage sees none of
this.

### 14.2 No rule passes a depfile

**CLOSED** by `mcpp:plugins` 0.4.0: six of six rules pass one, each spelling
measured against the tool rather than read from its help text, and a CI step
whose denominator is `ls rules/*.cppm` so a seventh rule is counted the day it
is added rather than the day someone remembers the step.

Verified in the same sandbox run, against the published packages: editing a file
the shader `#include`s, and nothing else, moved the embedded payload
(`PASS: an included file reaches the artifact (1884 -> 1868)`).

`mcpp::action::depfile` shipped in 2026.9.7.1 and, through `mcpp:plugins`
0.3.0, `grep depfile rules/` returned nothing across all six rules; each
declared `a.input(source)` alone. A shader or kernel that `#include`s another file does
not rebuild when that file changes, which is the same behaviour xmake has and
the one thing CMake's `add_custom_command(DEPFILE)` gets right.

Every compiler involved already emits one: `glslangValidator --depfile`,
`glslc -MD -MF`, `slangc -depfile`, and `-MD -MF` for the clang-family drivers
behind cuda, hip, sycl and ascendc. The work is one flag and one
`a.depfile(...)` per rule, plus a fixture whose criterion is that editing an
included file rebuilds -- the criterion has to be the artifact's content, not
the build's exit code, because the defect is a green build over stale bytes.

14.1 and 14.2 belong in one release of `mcpp:plugins`. They are the same class
of defect -- a green build over stale bytes -- and the same fixture shape
answers both: edit a file the compile reads, and assert on what got embedded.

They are NOT the same fix, and 4.3's note above is what it cost to believe they
were. A depfile carries what a command reports about its own input; the other
carries what nothing reports. Two channels, and the reason for two is measured
in 14.1 rather than assumed.

### 14.3 A device on Windows, and who decides it

The cause is settled and it is not a packaging defect. Under `VK_LOADER_DEBUG=all`
on a GitHub runner:

```
INFO: Loader is running with elevated permissions.
      Environment variable VK_DRIVER_FILES will be ignored
INFO: Loader is running with elevated permissions.
      Environment variable VK_ICD_FILENAMES will be ignored
DRIVER: Found no registry files in HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\Vulkan\Drivers
ERROR | DRIVER: Registry lookup failed to get ICD manifest files.
```

A process running elevated is not permitted to be told where its drivers are,
because a path a non-administrator can write would then decide what code an
elevated process loads. The loader falls back to the registry, which on a
machine with no GPU is empty.

**The registry entry, exactly.** From `loader/loader_windows.c` and
`vk_loader_platform.h` of the loader `compat:vulkan` builds:

- key `HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\Vulkan\Drivers`
- value NAME: the absolute path of the ICD manifest, `...\lvp_icd.x86_64.json`
- value DATA: `REG_DWORD` `0` -- the loader accepts an entry only when
  `value_size == sizeof(value) && value == 0`
- a driver whose file name is not in the loader's `known_drivers` table skips
  the DXGI adapter check and is "assumed to be active", which is what lets a
  software rasteriser be listed at all

**Three ways to get there, and they are not equivalent.**

1. `xim:mesa-lavapipe`'s `config()` writes the entry on Windows. It has the
   information and the install already runs with the necessary rights on a
   runner. It is also a MACHINE-WIDE mutation performed by a package install:
   every Vulkan application on that machine would then see lavapipe. That is a
   different promise from the one the package makes today, whose own comment
   says it "places the payload and stops; naming the ICD is the consumer's".
2. The consumer writes it -- this repository's CI step, or a project's own
   `build.mcpp`. Scoped to whoever wants it, and it keeps the package's promise
   intact; the cost is that every consumer repeats it, which is the shape the
   rule-package work exists to remove.
3. Run the program unelevated. Correct on a developer machine and awkward on a
   runner that is elevated by construction.

**Recommendation: 2 for CI now, and 1 only behind an explicit opt-in.** A
package that registers a driver system-wide as a side effect of being installed
is a surprise, and the surprise lands on software that has nothing to do with
mcpp. The decision is `xim:mesa-lavapipe`'s to make, not this repository's.

Whichever is chosen, the CI step withdrawn from `ci-windows.yml` carries the
four eliminations and the loader's own output beside it, so the next attempt
starts from here.

### 14.4 A rule feature that is on by default does not imply `host-module`

`host-module` is inferred from the features a consumer REQUESTS. A rule package
whose rule sits in its own `[features] default` is activated without being
named, so the rule modules are collected -- that reads the resolved set -- while
the inference reads the requested one. Measured with a probe package declaring
`default = ["rules-probe"]`: mcpp synthesises the build program and then refuses
with

```
error: build.mcpp imports 'probe.rules.probe', and no dependency provides it
       as a host module.
       declared without `host-module = true`: probe.rules
```

**The fix that was written and withdrawn.** Extending the inference to the
dependency's own defaults made the outcome worse: the refusal gained
`importable here: rules`, which says the dependency is wired up while the
feature's own module still is not.

**The mechanism is not what the first reading said, and that is worth
recording.** The withdrawal note guessed that a default feature's sources are
folded into the set `units()` enumerates too late. That is false: the fold
happens at `prepare.cppm` line ~7138 and `units()` runs at ~7616. So the cause
is elsewhere and is NOT established. Anyone picking this up should start by
finding it rather than by trusting the guess.

`mcpp:plugins` declares `default = []`, so nothing published reaches this, and
the current behaviour is a refusal that names both the module and the key to
add. It is a correctness-of-diagnosis item, not a correctness-of-artifact one.

### 14.5 A hermetic `vulkan-1.dll` on Windows

Measured feasible and not currently needed. The Khronos loader in
`compat:vulkan` cross-builds into a working DLL from the source the index
already carries: 265 exports matching upstream's `vulkan-1.def` name for name
and all `vk*`-prefixed, `DllMain` present, importing only ADVAPI32, CFGMGR32,
KERNEL32 and msvcrt. The descriptor's note argues a Windows loader must be a
DLL, not that it cannot be built.

**One constraint, found while sizing this and true of every descriptor.** A
platform table's `targets` APPENDS rather than replaces. `mcpp xpkg parse` on
today's `pkgs/c/compat.vulkan.lua` reports

```
targets: ["vulkan", "vulkan"]
```

-- the base `{ ["vulkan"] = { kind = "lib" } }` and the linux
`{ ["vulkan"] = { kind = "shared", soname = ... } }` are two targets sharing a
name, silently. So a Windows entry naming a target `vulkan-1` would produce a
THIRD target rather than a replacement. The change has to either move the base
`targets` into each platform table, so each platform declares exactly one, or
keep the name `vulkan` and reach `vulkan-1.dll` through `soname`.

That a duplicate target name is accepted without a word is worth a look on its
own, independently of this entry.

### 14.6 Not open: the Slang version pin

Recorded because it was on the plan and is resolved by fact rather than by work.
`xim:slang` publishes `2026.14.1` and nothing newer, `latest` points at it, and
`mcpp:plugins` declares `>=2026.14.1`. The floor is already at the index's
ceiling. Raising it means publishing a newer slang payload to `xim-pkgindex`
first -- four platforms and a GitCode mirror -- which is a packaging task, not a
pin edit.

### 14.7 The `--no-accel` diagnostic 5.6 specified was never written

Section 5.6 argued that an `import` of an accelerator-produced module from a
translation unit that is not `cfg`-gated is a diagnosable condition, and section
8's stage C said to add it in the same change as the surface. The surface
shipped; the diagnostic did not, and nothing recorded that -- so between 0.3.0
and this entry it was neither done nor open, which is the worse of the two
states because only one of them gets looked at again.

The failure it would report is real and its shape is known: under `--no-accel`
the constrained glob is left out, the rule generates nothing, and a TU importing
the generated module fails to compile. The author meets this in CI rather than
locally, and the compiler's message names a module that does not exist rather
than the reason it does not.

What is needed is one check at the point where the module graph and the `cfg`
gates are both in hand: an import of a module that a rule produces only under an
accelerator, from a unit compiled in every configuration, is refused naming the
importing file, the module and the seam pattern that resolves it. The engine
must not know which rule produced what -- so the fact "this module is produced
only when an accelerator is named" has to come from the action that declares it,
via `mcpp::action::provides`, and not from a table of rule names.

Every converted example already uses the seam, so nothing in the tree reproduces
this today. That is a reason to write the fixture first.

**Left open deliberately in the 0.4.0 batch, and what the investigation found.**
There is no single mcpp-side site to improve. An ordinary translation unit that
imports a module nothing provides does not reach an mcpp diagnostic at all: the
failure is the compiler's, `failed to read compiled module`. Adding one means
detecting "an import with no provider" in the module graph and refusing there --
a good message in general, and a change whose blast radius is every import
resolution in every project, including the ones that resolve through headers,
`std`, and host modules. A hasty version would refuse builds that are correct,
which is worse than the message being poor.

The other half is that the engine must not learn WHICH rule produces a module
under WHICH accelerator; that knowledge belongs to the packages, and the whole
design rests on the engine not holding it. What can carry it is
`mcpp::action::provides`: an action that declares a module and is submitted only
under an accelerator is a fact the engine can see without knowing what an
accelerator is. That is the shape to build, and it is a batch of its own.

### 14.8 L0 does not exist, and the ladder has three rungs rather than four

Recorded so that section 6 is not read as a description. `[rules]` was withdrawn
and is not planned (13.1.2), so the L1 rung as spelled there will not be built;
tuning lives at L2, on the rule's `options`. L0 -- `[build] accel` plus a glob,
no `build.mcpp` and no `[build-dependencies]` -- was never started, and stage D
in section 8 was where it was to be done.

It remains the largest single improvement to first use, and the mapping it needs
is already computed: `src/build/prepare.cppm` prints extension-to-rule-package
in the diagnostic for a device source no rule claimed. Nothing reads it. That is
the shape this project has recorded before -- a value that is derived correctly
and then wired to no decision -- and lifting it out of the diagnostic string is
most of the work.

The cost is an engine one, and it is the reason this is a register entry rather
than a task: the engine would have to name rule packages it currently does not
know, which is the one property section 1 credits the design with. L0 has to be
built so that the mapping is still supplied by the packages -- through the index
rather than through a table in the engine -- or it buys first-use convenience
with the property that made a new device language cost no engine release.
