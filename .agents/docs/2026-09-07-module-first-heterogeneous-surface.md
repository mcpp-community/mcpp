# A module-first surface for graphics acceleration and heterogeneous computing

Date: 2026-09-07. Base: mcpp `81358046`, `mcpp-plugins` `e75e4f5` (0.2.5).

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

Still open, and load-bearing for what follows:

- `mcpp::action` has no depfile field. Its fields are `id`, `role`,
  `description`, `blocking`, and the `input`/`output`/`arg`/`provides`/
  `imports`/`target` setters. Inputs are fixed at submission.
- `glslc` cannot be selected by declaration; `find_compiler` consults
  `xpkg_dir("glslang")` before `xpkg_dir("shaderc")` and the rule declares
  glslang unconditionally, so glslang wins in every build that works.
- Nothing generates a module. `mcpp.tools.embed` and `mcpp.rules.spirv` both
  emit headers that the consumer includes by name.

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

The user makes no new decision, and the default cannot be wrong. `[rules]` can
override it in either direction.

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

## 7. What remains open, and where each lands

1. **No dependency tracking for included device sources.** `mcpp::action` has no
   depfile field, so `rules/spirv.cppm` declares only the `.comp` and
   `rules/cuda.cppm` only the `.cu`. Editing a `.glsl` or a `.cuh` rebuilds
   nothing and the build is green. All three shader compilers already emit a
   depfile (3.5). This is the only open item that produces a wrong artifact
   rather than a failure, and the `.incbin` dependency in 4.3 needs the same
   field.

2. **`glslc` cannot be selected by declaration.** `find_compiler` consults
   `xpkg_dir("glslang")` before `xpkg_dir("shaderc")`, and the rule declares
   glslang unconditionally, so glslang wins in every build that works. The
   rule's own "no shader compiler found" diagnostic advises naming
   `xim:shaderc` and says it will win, which holds only when neither is
   installed. The project's CI corroborates: the glslc step downloads a tarball
   and sets `MCPP_GLSLC` rather than declaring the payload.

3. **The stem refusal becomes narrower once 5.3 lands.** It should stay for a
   genuine within-directory collision and stop firing for two directories.

4. **Slang.** `slangc` should become `mcpp.rules.slang` rather than a third
   flavour of `rules.spirv`: glslang and glslc are two drivers for one language
   compiling the same `.comp`, whereas Slang is a different language with a
   different extension and a target set that includes DXIL and Metal, for which
   the Vulkan axis `parse_target` reads has no answer. Its module dependency
   graph needs no new concept -- a shader module package is an ordinary mcpp
   package whose `include_dirs` names its `.slang` directory. `vulkan-rt` had to
   invent a scope API for this because xmake offered no package-level dependency
   to reuse; mcpp has one, and should use it rather than copy the workaround.

## 8. Staging

Each stage is useful alone and none depends on a later one.

**A. A depfile field on `mcpp::action`.** The only open defect that yields a
wrong artifact with a green build. Every compiler involved already emits one.
Fixing it at the action rather than in one rule serves `cuda`, `spirv` and 4.3
at once. Independent of everything else here.

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

## 12. Slang, in full

Section 7 argued that `slangc` belongs in a rule of its own. This section states
what that rule is.

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
