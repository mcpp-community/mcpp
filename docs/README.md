# User Documentation

**English** | [简体中文](zh/README.md)

This tree is the **usage manual for what mcpp implements**. Each chapter states
what a capability does, how it is written, and what its current limits are. The
reasoning behind a design, the alternatives that were rejected, and work that is
planned rather than shipped are deliberately absent — they belong to the design
records, which are not user documentation.

## Where a document lives

| tree | reader | what it holds |
|---|---|---|
| `docs/**` | someone with a task in hand | how to use what mcpp implements |
| [`docs/specs/**`](specs/README.md) | someone implementing against a mechanism: index authors, downstream tools, contributors | semantics, constraints and matching rules, each tagged with its implementation status |
| `.agents/docs/**` | whoever made a change, and whoever later asks why it is that way | the reasoning, the measurements, and what was refuted |
| `.agents/skills/**` | a contributor or agent following a procedure | ordered steps with criteria |

A chapter cites a specification for exact semantics. It does not cite a design
record: a record describes a moment and carries no stability promise, so
anything a reader needs is written here or in a specification instead.

## Start here

| To | Read | Run |
|---|---|---|
| write a program | [00](00-getting-started.md), [02](02-mcpp-toml.md) §1 | [`01-hello`](../examples/01-hello/), [`02-with-deps`](../examples/02-with-deps/) |
| write a library others import | [11](11-publishing-a-library.md), [04](04-features-and-capabilities.md), [02](02-mcpp-toml.md) §2.4 | [`04-workspace`](../examples/04-workspace/), [`11-features`](../examples/11-features/) |
| publish it | [10](10-pack-and-release.md), [11](11-publishing-a-library.md), [12](12-binary-distribution.md) | [`03-pack-static`](../examples/03-pack-static/), [`05-lib-distribution`](../examples/05-lib-distribution/) |
| build for another machine | [21](21-the-target-triple.md), [24](24-openkal-cross.md), [30](30-baremetal.md) | [`06-openkal-cross`](../examples/06-openkal-cross/), `mcpp new … --template riscv-virt-rt` |
| use a GPU or an accelerator | [32](32-heterogeneous-builds.md), [31](31-devices.md) | [`09-heterogeneous`](../examples/09-heterogeneous/), starting at [`boundary/`](../examples/09-heterogeneous/boundary/) |
| add a rule, a language or a generator | [40](40-authoring-a-rule-package.md), [05](05-build-mcpp.md) | [`08-build-rules`](../examples/08-build-rules/), [`12-a-new-device-language`](../examples/12-a-new-device-language/) |
| add a package to the index | [11](11-publishing-a-library.md), [SPEC-001](specs/package-identity.md) | [06](06-commands-by-scenario.md) — the publishing scenarios |
| change mcpp itself | [90](90-build-from-source.md), [92](92-release.md), [51](51-supported-versions.md) | — |

Lessons also arrive as project templates, which a package ships and `mcpp new
--template` instantiates. `riscv-virt-rt` (bare metal) and `ocornut.imgui`
(a graphical application) are the two documented today; the chapter that uses
one names it.

## Chapters

The number says which part a chapter is in: `0x` uses mcpp, `1x` ships what was
built, `2x` is toolchains and targets, `3x` is bare metal and devices, `4x`
extends mcpp, `5x` is the machine-facing contracts, `9x` is mcpp itself. Within
a part the order is a reading order.

### 0x — Using mcpp

- [00 — Getting Started](00-getting-started.md)
- [01 — Examples](01-examples.md)
- [02 — The mcpp.toml Manifest](02-mcpp-toml.md)
- [03 — Workspaces](03-workspace.md)
- [04 — Features and Capabilities](04-features-and-capabilities.md)
- [05 — Build Programs: `build.mcpp`](05-build-mcpp.md)
- [06 — Commands by Scenario](06-commands-by-scenario.md)

### 1x — Shipping what was built

- [10 — Packaging an Application for Release](10-pack-and-release.md)
- [11 — Publishing a Library to mcpp-index](11-publishing-a-library.md)
- [12 — Distributing a Prebuilt Library](12-binary-distribution.md)

### 2x — Toolchains and targets

- [20 — Toolchain Management](20-toolchains.md)
- [21 — The Target Triple](21-the-target-triple.md)
- [22 — The Target Side](22-target-side.md)
- [23 — The Project Environment](23-the-project-environment.md)
- [24 — Cross-Compilation Over openkal](24-openkal-cross.md)

### 3x — Bare metal, devices and accelerators

- [30 — Bare-Metal and Freestanding Targets](30-baremetal.md)
- [31 — Reaching a Device](31-devices.md)
- [32 — Heterogeneous Builds](32-heterogeneous-builds.md)

### 4x — Extending mcpp from outside

- [40 — Authoring a Rule Package](40-authoring-a-rule-package.md)

### 5x — Machine interfaces and compatibility

- [50 — Machine-Readable Output](50-machine-output.md)
- [51 — Supported Versions and Compatibility](51-supported-versions.md)

### 9x — Contributing to mcpp itself

- [90 — Building from Source and Contributing](90-build-from-source.md)
- [91 — Toolchain Internals](91-toolchain-internals.md)
- [92 — Releasing mcpp](92-release.md)

## Specifications

Normative documents — semantics, constraints and matching rules, each rule
tagged with its implementation status. For index authors, contributors and
downstream tooling.

- [specs/](specs/README.md) — index of all specs
  - [SPEC-001 — Package identity, `[dependencies]` selectors and matching](specs/package-identity.md)
  - [SPEC-002 — The target side: reserved namespace, five layers, three rules](specs/target-side.md)
  - [SPEC-003 — The exit-code contract](specs/exit-codes.md)
  - [SPEC-004 — `mcpp.toml` planes, conditioning shape, resolution axes and naming](specs/manifest-semantics.md)
