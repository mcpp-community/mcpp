# 31 — Authoring a Rule Package

**Reader:** an ecosystem author packaging a build step so that other projects
can use it — a device language, a shader compiler, a generated interface, a
check.

**The question this chapter answers:** how does a package supply a rule, and
what does a consumer have to write to use it.

**Not here:** adding a step to one project's own build, which is
[30 — Build Programs](30-build-mcpp.md) and is the same primitives at a smaller
scale; the feature keys themselves, which are
[06](06-features-and-capabilities.md); and the spellings of the shipped rules,
which belong to `mcpp:plugins`. Examples:
[`08-build-rules`](../examples/08-build-rules/) checks and embeds;
[`12-a-new-device-language`](../examples/12-a-new-device-language/) adds a
language.

## The extension model

mcpp's build surface is extended from packages rather than from releases. Five
points do the extending, and the engine holds no name that comes through any of
them.

| extension point | effect | declared on |
|---|---|---|
| `mcpp::action` | one edge in the build graph: a command with declared inputs and outputs | a build program, or a rule module it imports |
| `device_extensions` | an extension the engine classifies as a **device source** instead of refusing it | a feature of a package |
| `rule_module` | the module a consumer's build program imports to reach the rule | the same feature |
| `tools = [...]` | a generator or compiler **built from source for the build machine**, reached with `mcpp::dep_bin` | a dependency edge |
| `[xlings]`, `[feature-xlings]` | a prebuilt tool the rule runs, installed on demand | the package, or one of its features |

What the ecosystem has built out of them:

| surface | package | points used |
|---|---|---|
| CUDA, HIP, SYCL, Ascend C | `mcpp:plugins`, one feature each ([42](42-heterogeneous-builds.md)) | actions driving a vendor compiler, plus payloads gated on the accelerator |
| Slang | `mcpp:plugins`' `rules-slang` | `device_extensions = [".slang"]` — the first language mcpp supports without naming it in the engine |
| GLSL and HLSL to SPIR-V, and the module over the result | `mcpp:plugins`' `rules-spirv` | one action per shader, plus a generated module |
| the island boundary between a device and C++ | `mcpp.tools.island` | a generator, plus `mcpp::generated` |
| an asset as a linkable object | [`08-build-rules`](../examples/08-build-rules/) | `role = "object"` |
| a check that can fail the build | [`08-build-rules`](../examples/08-build-rules/) | `role = "check"` |
| a language the engine has never heard of | [`12-a-new-device-language`](../examples/12-a-new-device-language/) | `device_extensions`, plus a compiler built through `tools = [...]` |

### The shapes the model expresses

**A new language, whatever compiles it.** A rule claims the extension, submits
one action per source, and declares the compiler among that action's inputs.
The compiler may be a vendor toolkit, an LLVM front end, an interpreter that
emits a device binary, or a program the rule package builds from source. Whether
it produces a device binary, an object or C++ is the action's `role` and nothing
else. The engine never learns the language: it learns that an extension is a
device source and that an action claims it.

**Preprocessing and code generation.** An action with `role = "source"` produces
C++ that the declaring package then compiles, and every compile edge of that
package waits for it. The input can be a template, an interface definition, a
table, or another action's output — chaining is ordinary, because actions are
ordered and fingerprinted by their files.

**A file that is partly C++ and partly another language.** Classification
happens before any rule runs, so a file with an extension the engine owns is
compiled as C++ and never reaches a rule. A source carrying a foreign block
therefore uses an extension the rule claims, and the rule splits it: the C++ it
extracts goes through `role = "source"`, the foreign half through its own
compiler, and the seam between the two is the `extern "C"` boundary of
[42 — Heterogeneous Builds](42-heterogeneous-builds.md). No package in this
repository ships that shape today.

### The boundary

**A declaration cannot reclassify what the engine already owns.** A dependency's
`device_extensions` is consulted *after* the built-in roles, so a rule package
cannot claim `.cpp`, `.cppm`, `.c` or `.S`. Those are the engine's own
vocabulary, and a package must not be able to move a file out of it. Claiming
one is not diagnosed and has no effect: measured by adding `".cpp"` to a rule's
`device_extensions`, after which the consumer's `main.cpp` was still compiled as
C++ and the build succeeded.

**Module-interface extensions are the project's axis, not a rule's.** A project
that spells its interfaces `.ixx` declares `[build] module_extensions`
([04 — The mcpp.toml Project File](04-mcpp-toml.md)). No rule-package key adds
one, because a module interface is scanned for imports, produces a BMI and joins
the link — three engine behaviours rather than a command to run.

**An extension in neither table is refused by name**, which is why a mistyped
`device_extensions` surfaces at once instead of dropping a source:

```
error: scanner errors:
  .../orphan.zzz: 'orphan.zzz' is listed in [build] sources, and mcpp has no role
  for the extension '.zzz'.
  Its object would be compiled and then linked by nothing, so this is refused rather
  than built.
```

## The definition of a rule package

Three parts, and none of them is special to mcpp:

| part | content |
|---|---|
| a package | an ordinary `mcpp.toml` with a version and a licence |
| a module | a `.cppm` exporting `options` and a function that submits build edges |
| a feature | the switch that selects it, and the place its own dependencies hang |

The engine holds no rule names and no vendor names. A consumer names the
package, activates a feature, and the build program imports the module.

## The manifest: three keys

```toml
[features]
default = []

[features.rules-toy]
sources           = ["src/rules-toy.cppm"]
rule_module       = "example.rules.toy"
device_extensions = [".toy"]
```

| key | effect on a consumer |
|---|---|
| `sources` | the module is compiled only when the feature is active |
| `rule_module` *(2026.9.7.1+)* | the module the consumer's build program imports. It implies `host-module = true` |
| `device_extensions` *(2026.9.7.1+)* | those extensions are classified as **device sources**: never scanned for imports, never producing a BMI, and refused if no action claims them |

A rule that generates or checks rather than compiling a language declares no
`device_extensions`; `08-build-rules` is that shape and its consumer writes
`host-module = true` on the dependency edge itself.

Device extensions are not in the default source glob. A consumer opts in by
naming the files:

```toml
[build]
sources = ["src/*.cpp", "src/kernels/*.toy"]
```

## The module a consumer imports

The convention every shipped rule follows is an `options` struct with defaults,
and a `compile` (or `generate`) function that submits the edges:

```cpp
export module example.rules.toy;
import std;
import mcpp;

export namespace example::rules::toy {

struct options {
    std::string out_dir  = std::string(mcpp::out_dir());
    std::string rule_dir = std::string(mcpp::dep_dir("rules-toy"));
};

bool compile(options opt = {});

}
```

A rule that plans its edges in one function and submits them in another gives a
consumer a way past its last knob without hand-writing the action:
`08-build-rules`'s `plan` / `submit` pair is that shape.

**A rule takes the extensions it claims and leaves the rest.**
`mcpp::device_sources()` is one string, one package-root-relative path per line,
and it holds the package's **whole** device set. A build with two backends puts
both backends' sources in that one list, and every rule in the build program
reads the same value.

## Declaring work: `mcpp::action`

```cpp
mcpp::action a;
a.id          = id.c_str();          // stable, unique within the package
a.role        = "source";
a.description = desc.c_str();
a.arg("sh").arg(script.c_str()).arg(input.c_str()).arg(output.c_str());
a.input(input.c_str());
a.input(script.c_str());
a.output(output.c_str());
a.submit();
```

The strings must outlive the action. `a.id = ("toy:" + stem).c_str()` hands it a
pointer into a temporary that is gone by `submit()`.

### The four roles

`role` decides where the edge's outputs go and when the edge runs.

| `role` | outputs | ordering |
|---|---|---|
| `source` | compilable ones join the compile set | every compile edge of the declaring package waits for them |
| `check` | a stamp file | runs alongside compilation; `blocking = true` makes compiles wait |
| `object` | join the **link** set | the link edge consumes them |
| `artifact` | a new file | its inputs are link outputs, so it runs after the link |

### Declared inputs, and the compiler among them

An action re-runs when a declared input changes. **The tool the command invokes
is an input.** Without it, editing the rule's own compiler leaves every edge
clean and the artifact keeps the bytes the previous compiler produced — a green
build over a stale result.

An action's command runs from the **build directory**, not from the package
root. `mcpp::device_sources()` answers package-root-relative, so a rule joins
`mcpp::manifest_dir()` to each path before putting it on a command line.

### A depfile, when the command discovers its own inputs

A shader or a kernel that includes another file has inputs the rule cannot
enumerate. Every compiler involved emits a depfile —
`glslangValidator --depfile`, `glslc -MD -MF`, `slangc -depfile`, and
`-MD -MF` for the clang-family drivers:

```cpp
a.depfile = dep.c_str();          // a path the command writes
a.arg("--depfile").arg(dep.c_str());
```

The depfile must not also be declared as an `output()`.

### Chaining actions

One action may consume what another produced. The engine orders and fingerprints
them, and that is the whole engine-side content of a device link: N `artifact`
actions whose outputs stay out of the link, and one `object` action that reads
them and produces the object that joins it.

## The environment a rule brings with it

A rule owns the list of packages it drives, because it is the code that runs the
compiler and puts the library directory on the link line.

```toml
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-cuda]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"
```

Two gates, and both must open before a byte is downloaded: the feature says
whether the rule is wanted, and the `cfg(accelerator = ...)` selector says
whether this build compiles for the device. A build with no accelerator opens
neither.

A bare version is a **choice** a project may override; `>=` is a
**requirement** a project may not go below. See *One package, one version* in
[23 — The Project Environment](23-the-project-environment.md).

## An island's boundary is generated, and not by a rule

`mcpp.tools.island` writes the `extern "C"` header a device translation unit
includes and the module the C++ side imports. It is **not** a rule package and
nothing in `mcpp:plugins` calls it: a project calls it from its own
`build.mcpp`, because the module name and the shape of the namespace are the
project's decisions rather than the rule's.

What a rule owes it is one field. The generated header reaches the island
through `mcpp::tools::island::force_include_flags`, and those flags go on the
device compiler's own command line -- `options::flags` on `cuda`, `hip`, `sycl`
and `ascendc` -- because that driver inherits nothing from `mcpp::cxxflag`, and
because forcing a header into every C++ translation unit would put declarations
ahead of a module interface's `export module` line, which is ill-formed.

See [42 — Heterogeneous Builds](42-heterogeneous-builds.md) under *Generating
the boundary* for the generator itself.

## Reporting what the build should know

### `warning` — succeeding and still being heard

`mcpp::warning` is the channel for a rule that finished its job and found
something worth saying: a host compiler it had to choose, a payload it fell back
to. A build program's output is otherwise printed only on a non-zero exit.

### `fact` / `floor` — the probe channel

A rule measures, and the engine compares before anything is compiled.

```cpp
mcpp::fact("cuda.driver", driver_version);
mcpp::floor("cuda.driver", runtime_needs);
```

The engine reads a name, a relation and a version; the name is data flowing
through it. A probe that reaches no answer states none.

## Finding the rule's own files

| object | accessor |
|---|---|
| the rule package's own tree | `mcpp::dep_dir("<name>")` — under the name the **consumer** declared in `[dependencies]` |
| a payload declared under `[xlings.workspace]` | `mcpp::xpkg_dir("<name>")` |
| a host tool built from a dependency | `mcpp::dep_bin("<pkg>", "<tool>")` |

`dep_dir` answers under the spelling in the consumer's manifest. A rule that
ships files beside its module exposes the directory as an option so a consumer
that declares the edge under another key can supply it, and refuses with a
message naming what it looked for rather than running a command with an empty
path.

## Authoring a distribution member (mcpp 2026.9.11.1+)

A `dist-*` member is neither a rule nor a tool. It does not compile a
translation unit and it does not do its work while the build program runs: it
consumes **link outputs** and produces something a user installs — an `.msi`, a
`.deb`, an AppImage, a signed `.app`. The mechanism is an `artifact` action and
`mcpp pack --format <name>`, documented in
[30 — Producing a distributable](30-build-mcpp.md#producing-a-distributable-pack_format--stage_dir-20269111).

Everything in this chapter applies unchanged. Six things bind harder here, and
each is a mistake this category makes and a rule does not.

**Declare unconditionally, submit conditionally.** `provides_pack_format` is
what the engine reads to answer "which formats does this graph provide", on a
build that asked for none. A member that declares only when asked works for its
author, who always passes their own format, and makes the set unknowable for
everyone else.

**Expose a plan and a submit.** A distributable is the last thing before a
user's hands, so it is the most likely part of a build to need a
project-specific edit: a different compression level, one extra file, a second
signature. `generate_all(opt)` being exactly `submit(plan_all(opt))` is what
keeps such an edit from becoming a reimplementation of the member.

**Name the input; do not harvest a directory.** A path that resolves to nothing
is silent, and a named input that is missing is an error. Measured: a WiX action
that bound a directory and harvested it produced a **valid, empty, 52 KB
installer with no diagnostic** when the path resolved to nothing on Windows.
`${mcpp.target_file:<name>}` is the mechanism — a build program is told neither
the triple nor the fingerprint, and an unknown target name is refused rather
than expanded to an empty path.

**Assert a floor on the member's own output, on the success path.** Where a
member can tell that its result is empty or implausible, it must say so through
`mcpp::warning`, because stderr on a successful build is discarded. This is the
same failure the paragraph above measured, caught one layer later.

**Declare the tool where it will be looked up.** `xpkg_dir` answers from
`MCPP_XPKG_*_DIR`, which mcpp sets for the package *being built*. A
dependency's declaration provisions the payload without making it visible to a
consumer's build program, so a member that runs a payload tool declares it
itself — and says so when the lookup returns empty, rather than pointing at a
manifest the reader does not own.

A host module's declaration is the exception, and it is visible in every build
program it is compiled into (`tests/e2e/622`), because a host module's own
code runs as part of each of those build programs rather than as a dependency
seen only through the graph. This is why a member declares its own payload on
itself, and a project that pulls the member in never repeats the declaration.
A dist member's payload therefore belongs on the member — on the target axis
when the tool serves one target and not others, exactly as any other
target-conditional declaration does:

```toml
[target.'cfg(env = "android")'.feature-xlings.dist-apk]
"xim:android-build-tools" = ""
```

**A build must not reach the network, and a wrapped tool may.** Measured on
`appimagetool` 1.9.1: it downloads its type-2 runtime stub from a GitHub
release on every invocation unless `--runtime-file` names a local copy. A member
that wraps such a tool has to supply the file from its declared payload.
Install time is when a download is legitimate; build time is not, and a build
that fetches is neither reproducible nor usable offline.

**One `(name, version)` names one payload.** A member that wraps a signing or
packaging tool inherits that tool's compatibility surface, so versioning in
lock-step with the wrapped tool is legitimate and says something true.

## Current limitations

- A rule feature that is in the package's own `[features] default` does not
  imply `host-module`. `mcpp:plugins` declares `default = []`; a rule package
  whose rule is on by default is refused with a message naming the module and
  the key to add.
- `mcpp emit xpkg` writes `manifest = "mcpp.toml"` into the `mcpp` segment, and
  `mcpp xpkg parse` reports that key as unknown and exits 1. No descriptor in
  `mcpp-index` uses it (0 of 218); a package keeping its own `mcpp.toml` omits
  the `mcpp` field entirely. See *Current limitations* in
  [09 — Commands by Scenario](09-commands-by-scenario.md).
