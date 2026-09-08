# 01 — Examples

The [`examples/`](../examples) directory is a curriculum. Each project is
runnable on its own, and each one teaches **one thing no earlier example
teaches**. This chapter says what that thing is, so you can enter at the level
you need rather than reading from the start.

## Running an example

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp/examples/01-hello
mcpp build && mcpp run
```

Every example ships a README that explains only what it adds. Installation and
toolchain setup live in [00 — Getting Started](00-getting-started.md) and are
not repeated.

## The curriculum

### A — The shape of a project

| example | first to teach |
|---|---|
| [`01-hello`](../examples/01-hello/) | a package, `import std`, `mcpp build` and `mcpp run` |
| [`02-with-deps`](../examples/02-with-deps/) | `[dependencies]`, the lock file, `mcpp add` |
| [`04-workspace`](../examples/04-workspace/) | `[workspace]`, path dependencies, `mcpp build --workspace` |
| [`11-features`](../examples/11-features/) | `[features]` **declared** rather than consumed, `[feature-deps]`, `[dev-dependencies]`, `[profile.<name>]`, `mcpp::has_feature` |

### B — Publishing

| example | first to teach |
|---|---|
| [`03-pack-static`](../examples/03-pack-static/) | `mcpp pack --mode static`, `[target.<triple>]`, `[pack]` |
| [`05-lib-distribution`](../examples/05-lib-distribution/) | a library's interface and its prebuilt binaries; a C header and a C++ module from one source |

### C — The environment

| example | first to teach |
|---|---|
| [`07-project-subos`](../examples/07-project-subos/) | `[xlings]`, `[xlings.workspace]`, a build program whose `PATH` is the environment the project declared |

### D — Targets

| example | first to teach |
|---|---|
| [`06-openkal-cross`](../examples/06-openkal-cross/) | `--target`, one source built for four machines from any host |

Bare metal is taught by a **template** rather than by a directory here — see
*Lessons that arrive as templates* below.

### E — Devices and graphics

Read [`09-heterogeneous`](../examples/09-heterogeneous/) in order. Its README is
the map; the table below is what each sub-example adds.

| example | first to teach |
|---|---|
| [`…/boundary`](../examples/09-heterogeneous/boundary/) | the island boundary alone: a generated module the consumer imports, with no seam and no header in the project. Needs no device |
| [`…/cuda`](../examples/09-heterogeneous/cuda/) | a device compiler, a seam over the generated boundary, the driver stated as a fact and a floor |
| [`…/vulkan`](../examples/09-heterogeneous/vulkan/) | a compute shader whose SPIR-V payload arrives as a module |
| [`…/sycl`](../examples/09-heterogeneous/sycl/) | a second compiler with its own standard library |
| [`…/hip`](../examples/09-heterogeneous/hip/) | the boundary written by hand — the contrast against `boundary/` and `cuda/` |
| [`…/cann`](../examples/09-heterogeneous/cann/) | a vendor outside the NVIDIA and Khronos lineages |
| [`…/multi-backend`](../examples/09-heterogeneous/multi-backend/) | several backends in one artifact, chosen when the program runs |
| [`10-graphics/offscreen`](../examples/10-graphics/offscreen/) | a rendering pipeline whose result is pixels, asserted against a software rasteriser |

### F — Authoring for the ecosystem

| example | first to teach |
|---|---|
| [`08-build-rules`](../examples/08-build-rules/) | two rule packages and a project using both; `host-module = true`, `mcpp::action` with `role = "check"` |
| [`12-a-new-device-language`](../examples/12-a-new-device-language/) | `device_extensions` and `rule_module`: a rule package teaching mcpp a language the engine has never heard of |

[23 — Authoring a Rule Package](23-authoring-a-rule-package.md) is the reference
these two illustrate.

## Lessons that arrive as templates

A package may ship `templates/<name>/`, which `mcpp new --template` instantiates.
That is a third teaching surface beside this directory and the chapters, and it
is where a lesson belongs when the thing being taught is owned by a package
rather than by mcpp.

| template | lesson | chapter |
|---|---|---|
| `riscv-virt-rt` | a bare-metal project, its board support and its runner | [13](13-baremetal.md) |
| `riscv-virt-rt:nolibc` | the same with no C library | [13](13-baremetal.md) |
| `ocornut.imgui` | a graphical application with its window and rendering stack | [03](03-toolchains.md) |

```bash
mcpp new blinky --template riscv-virt-rt
```

## Adding an example

An example directory is `mcpp.toml` + `src/` + `README.md`, numbered after the
last one. A new example is warranted when a capability **changes the shape of a
project** — the files it contains, the manifest it declares, or the commands its
author types. A capability that is one line inside a project an example already
contains belongs in that chapter as a code block; one reached only through a
command belongs in [21 — Commands by Scenario](21-commands-by-scenario.md).

The README states what the example is the first to teach and the criterion by
which it is judged to work. For contribution mechanics see
[04 — Building from Source & Contributing](04-build-from-source.md).
