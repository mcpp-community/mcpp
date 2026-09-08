# 02 — Scenarios

**Reader:** a developer deciding whether mcpp fits the work in front of them,
and which of its features that work will use.

**The question this chapter answers:** what kinds of project does mcpp serve,
and for one kind, which features are used and in what order.

**Not here:** the field reference, which is [04](04-mcpp-toml.md); the catalogue
of examples, which is [03](03-examples.md) and is indexed by example rather than
by scenario; and the command lookup, which is
[09](09-commands-by-scenario.md). This chapter is indexed by **the work**.

Each scenario states the situation, what mcpp contributes to it, the path
through the chapters, one project to run, and the one thing that surprises
people. Read only the scenario that matches; they do not build on each other.

## The scenarios

| | scenario | run |
|---|---|---|
| [1](#1-a-command-line-tool-or-service) | a command-line tool or a service | `examples/01-hello` → `03-pack-static` |
| [2](#2-a-library-other-projects-import) | a library other projects import | `examples/11-features`, `05-lib-distribution` |
| [3](#3-a-repository-with-several-packages) | a repository with several packages | `examples/04-workspace` |
| [4](#4-a-graphical-application) | a graphical application | `mcpp new … --template ocornut.imgui` |
| [5](#5-building-for-another-operating-system) | building for another operating system | `examples/06-openkal-cross` |
| [6](#6-a-target-with-no-operating-system) | a target with no operating system | `mcpp new … --template riscv-virt-rt` |
| [7](#7-compute-on-a-gpu-or-an-accelerator) | compute on a GPU or an accelerator | `examples/09-heterogeneous` |
| [8](#8-graphics-rendering) | graphics rendering | `examples/10-graphics/offscreen` |
| [9](#9-a-build-step-the-project-needs) | a build step the project needs, and sharing it | `examples/08-build-rules`, `12-a-new-device-language` |

## 1. A command-line tool or service

**The situation.** A program built from C++23 modules, with a few dependencies,
that has to run on a machine that does not have mcpp.

**What mcpp contributes.** `import std` works with no configuration; the
compiler is a pinned payload rather than whatever the machine has; and one
command produces a binary that carries what it needs.

**The path.**

1. [00 — How mcpp Works](00-how-mcpp-works.md) — the five nouns.
2. [01 — Getting Started](01-getting-started.md) — a program on the screen.
3. [05 — Dependencies and Resolution](05-dependencies.md) — `[dependencies]`,
   the lock file.
4. [08 — Testing](08-testing.md) — `tests/**/*.cpp`.
5. [10 — Packaging an Application](10-pack-and-release.md) — `mcpp pack`.

**Run.** [`examples/01-hello`](../examples/01-hello/), then
[`02-with-deps`](../examples/02-with-deps/), then
[`03-pack-static`](../examples/03-pack-static/).

**What surprises people.** A debug build and a release build do not invalidate
each other. Each configuration gets its own fingerprinted directory under
`target/`, so alternating between them is not a rebuild.

## 2. A library other projects import

**The situation.** Code that other packages will name in their
`[dependencies]`, possibly with optional parts.

**What mcpp contributes.** A module interface is the published surface; a
feature makes part of it optional without a second package; and a prebuilt
binary can state which toolchains it is compatible with.

**The path.**

1. [04 — The mcpp.toml Manifest](04-mcpp-toml.md) — `[lib]`, the library root.
2. [06 — Features and Capabilities](06-features-and-capabilities.md) — optional
   parts, and the dependencies they pull.
3. [08 — Testing](08-testing.md) — `[dev-dependencies]`.
4. [11 — Publishing a Library](11-publishing-a-library.md) — the descriptor.
5. [12 — Distributing a Prebuilt Library](12-binary-distribution.md) — if
   binaries ship too.

**Run.** [`examples/11-features`](../examples/11-features/) declares all three
shapes of feature; [`05-lib-distribution`](../examples/05-lib-distribution/) is
the producer and consumer pair.

**What surprises people.** The criterion for an optional backend is not that the
default build still works — it is that the default build's **resolution does not
name** the optional package. A dependency that is resolved and merely unused
still costs a download.

## 3. A repository with several packages

**The situation.** Several packages developed together, depending on each other
by path.

**What mcpp contributes.** One command builds or tests the set; a member
resolves its siblings without a registry; and each member keeps its own
manifest and identity.

**The path.**

1. [07 — Workspaces](07-workspace.md) — `[workspace]`, path dependencies.
2. [05 — Dependencies and Resolution](05-dependencies.md) — what a path
   dependency does and does not do.
3. [08 — Testing](08-testing.md) — the fan-out.

**Run.** [`examples/04-workspace`](../examples/04-workspace/).

**What surprises people.** A workspace member is not a root. Tooling that
enumerates "every package" has to say which of the two it means, and the
answer changes what gets built.

## 4. A graphical application

**The situation.** A desktop program with a window, a renderer and fonts.

**What mcpp contributes.** The window and rendering stack are ordinary
dependencies; a template scaffolds a working project; and what the artifact
needs at run time is declared rather than discovered.

**The path.**

1. [01 — Getting Started](01-getting-started.md) — `mcpp new --template`.
2. [05 — Dependencies and Resolution](05-dependencies.md) — the stack.
3. [10 — Packaging an Application](10-pack-and-release.md) — what ships beside
   the executable.

**Run.** `mcpp new myapp --template ocornut.imgui`.

**What surprises people.** On Linux an mcpp artifact runs behind a private
loader that does not consult `/usr/lib`. Anything the host must supply — a
graphics driver, a Vulkan ICD — is reached through an adapter package the
project declares, not by being installed on the machine.

## 5. Building for another operating system

**The situation.** One source tree that has to produce binaries for Linux,
Windows and macOS.

**What mcpp contributes.** The target is an argument, not a second checkout;
the cross toolchain is a payload; and a target this host cannot serve is
refused rather than quietly built for the host.

**The path.**

1. [21 — The Target Triple](21-the-target-triple.md) — how a target is named,
   and which host serves which.
2. [24 — Cross-Compilation Over openkal](24-openkal-cross.md) — the mechanism.
3. [22 — The Target Side](22-target-side.md) — when the manifest must differ
   per target.

**Run.** [`examples/06-openkal-cross`](../examples/06-openkal-cross/) — one
program built for four targets from any host.

**What surprises people.** `--target` does not require the machine to already
have that toolchain. What it does require is that the target be *servable* from
this host; the support matrix in [21](21-the-target-triple.md) says which are.

## 6. A target with no operating system

**The situation.** Firmware for a board or a microcontroller: no OS, no libc by
default, a linker script and a vector table.

**What mcpp contributes.** A board-support package supplies the whole target
world — the linker script, the startup code, and the **runner** — so `mcpp run`
is the same command on an emulator and on hardware.

**The path.**

1. [40 — Bare-Metal and Freestanding Targets](40-baremetal.md) — the target,
   the tiers, what of `std` survives.
2. [41 — Reaching a Device](41-devices.md) — runners, named runners.
3. [08 — Testing](08-testing.md) — tests that run on the board.

**Run.** `mcpp new blinky --template riscv-virt-rt`.

**What surprises people.** The emulator and the physical board are one package
and one feature apart, not two packages. `mcpp run --features hardware` moves
the default runner; the command a developer types does not change.

## 7. Compute on a GPU or an accelerator

**The situation.** Part of the program is a kernel compiled by a vendor's
compiler and linked into an ordinary binary.

**What mcpp contributes.** The device toolkit is declared by the rule package
and installed by the build; the accelerator is one axis stated once; and the
artifact records which devices it carries code for.

**The path.**

1. [42 — Heterogeneous Builds](42-heterogeneous-builds.md) — `accel`, the
   island, the seam.
2. [30 — Build Programs](30-build-mcpp.md) — how a rule reaches the graph.
3. [06 — Features and Capabilities](06-features-and-capabilities.md) — the
   feature that selects a lane.

**Run.** [`examples/09-heterogeneous/boundary`](../examples/09-heterogeneous/boundary/)
first — it needs no device — then
[`…/cuda`](../examples/09-heterogeneous/cuda/).

**What surprises people.** A build that names no accelerator downloads nothing.
Two gates must open before a byte of a multi-gigabyte toolkit is fetched: the
feature that selects the rule, and the `cfg(accelerator = …)` selector that says
this build actually compiles for the device.

## 8. Graphics rendering

**The situation.** Shaders compiled to SPIR-V, a pipeline, and pixels that have
to be right.

**What mcpp contributes.** The shader compiler is a declared payload; compiled
shaders arrive as a **module** rather than as a generated header nobody named;
and a rendering result can be asserted without a GPU.

**The path.**

1. [42 — Heterogeneous Builds](42-heterogeneous-builds.md) — the shader lane.
2. [30 — Build Programs](30-build-mcpp.md) — the one line that asks for the
   module surface.
3. [10 — Packaging an Application](10-pack-and-release.md) — shipping it.

**Run.** [`examples/10-graphics/offscreen`](../examples/10-graphics/offscreen/) —
renders a triangle offscreen and compares the pixels against a software
rasteriser.

**What surprises people.** A software device is not automatically a substitute
for hardware. A framework may reject one on its device *type* even when it
advertises every feature the framework requires, and that is the framework's
policy rather than a packaging defect.

## 9. A build step the project needs

**The situation.** Code generation, an embedded asset, a check, or a second
compiler — something the engine has no rule for.

**What mcpp contributes.** The step becomes edges in the same graph as
everything else: ordered, fingerprinted, incremental, and reported by name when
it fails. It is not a pre-build script.

**The path.**

1. [30 — Build Programs](30-build-mcpp.md) — `mcpp::action`, the four roles.
2. [31 — Authoring a Rule Package](31-authoring-a-rule-package.md) — if other
   projects should use the step too.
3. [23 — The Project Environment](23-the-project-environment.md) — declaring
   the tools the step runs.

**Run.** [`examples/08-build-rules`](../examples/08-build-rules/) for a rule
that checks and embeds;
[`12-a-new-device-language`](../examples/12-a-new-device-language/) for one that
teaches mcpp a language the engine has never heard of.

**What surprises people.** The tool the step invokes is an **input** of the
step. Without that, editing the generator leaves every edge clean and the
artifact keeps the bytes the previous generator produced — a green build over a
stale result.

## Current limitations

- The scenarios here are the ones with a runnable project or a published
  template behind them. A scenario mcpp serves but nothing in this repository
  demonstrates is not listed, because a path with no project to run is a claim
  rather than a scenario.
- Adopting mcpp inside an existing build system is not a scenario here. mcpp
  builds a project it owns; interoperating with another build system's outputs
  is not documented and is not covered by any example.
