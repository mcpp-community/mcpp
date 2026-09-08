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

## What a rule package is

Three parts, and none of them is special to mcpp:

| part | what it is |
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
[04 — mcpp.toml](04-mcpp-toml.md).

## Generating an island's boundary

A device translation unit cannot import a module, so the boundary between it and
the C++ side is an `extern "C"` header. `mcpp.tools.island` from `mcpp:plugins`
reads the entry points marked `MCPP_EXPORT_C` out of both implementations and
writes that header and a module over it, so each signature exists once.

```cpp
mcpp::tools::island::options opt;
opt.module_name = "myapp.kernels";
opt.out_dir     = std::string(mcpp::out_dir()) + "/island";

const auto entries = mcpp::tools::island::scan(halves, opt);
const auto out     = mcpp::tools::island::emit(*entries, opt);
mcpp::generated(out->interface_file.c_str());
```

Four rungs are available, and each overrides the one above:

| rung | written by hand | the consumer writes |
|---|---|---|
| L0 | nothing but the marked entry points | `import myapp.kernels` — the island's own C-shaped interface |
| L1 | a seam module over the generated one | `import myapp.saxpy` — the interface the project designed |
| L2 | a seam, plus the entry list passed to `emit` directly | the same, for entry points a scan cannot see |
| L3 | the header and the module | the same, with the signature written twice |

The generated header reaches the island through
`mcpp::tools::island::force_include_flags`, whose flags go to the **rule** that
drives the device compiler rather than through `mcpp::cxxflag` — forcing a
header into every C++ translation unit puts declarations ahead of a module
interface's `export module` line, which is ill-formed.

[`examples/09-heterogeneous/boundary`](../examples/09-heterogeneous/boundary/)
is L0 and states what each rung costs.

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

| what | how |
|---|---|
| the rule package's own tree | `mcpp::dep_dir("<name>")` — under the name the **consumer** declared in `[dependencies]` |
| a payload declared under `[xlings.workspace]` | `mcpp::xpkg_dir("<name>")` |
| a host tool built from a dependency | `mcpp::dep_bin("<pkg>", "<tool>")` |

`dep_dir` answers under the spelling in the consumer's manifest. A rule that
ships files beside its module exposes the directory as an option so a consumer
that declares the edge under another key can supply it, and refuses with a
message naming what it looked for rather than running a command with an empty
path.

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
