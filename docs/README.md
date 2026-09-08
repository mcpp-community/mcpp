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
| write a program | [01](01-getting-started.md), [03](03-mcpp-toml.md) §1 | [`01-hello`](../examples/01-hello/), [`02-with-deps`](../examples/02-with-deps/) |
| write a library others import | [11](11-publishing-a-library.md), [05](05-features-and-capabilities.md), [03](03-mcpp-toml.md) §2.4 | [`04-workspace`](../examples/04-workspace/), [`11-features`](../examples/11-features/) |
| publish it | [10](10-pack-and-release.md), [11](11-publishing-a-library.md), [12](12-binary-distribution.md) | [`03-pack-static`](../examples/03-pack-static/), [`05-lib-distribution`](../examples/05-lib-distribution/) |
| build for another machine | [21](21-the-target-triple.md), [24](24-openkal-cross.md), [40](40-baremetal.md) | [`06-openkal-cross`](../examples/06-openkal-cross/), `mcpp new … --template riscv-virt-rt` |
| use a GPU or an accelerator | [42](42-heterogeneous-builds.md), [41](41-devices.md) | [`09-heterogeneous`](../examples/09-heterogeneous/), starting at [`boundary/`](../examples/09-heterogeneous/boundary/) |
| add a rule, a language or a generator | [31](31-authoring-a-rule-package.md), [30](30-build-mcpp.md) | [`08-build-rules`](../examples/08-build-rules/), [`12-a-new-device-language`](../examples/12-a-new-device-language/) |
| add a package to the index | [11](11-publishing-a-library.md), [SPEC-001](specs/package-identity.md) | [08](08-commands-by-scenario.md) — the publishing scenarios |
| change mcpp itself | [90](90-build-from-source.md), [92](92-release.md), [51](51-supported-versions.md) | — |

Lessons also arrive as project templates, which a package ships and `mcpp new
--template` instantiates. `riscv-virt-rt` (bare metal) and `ocornut.imgui`
(a graphical application) are the two documented today; the chapter that uses
one names it.

## Chapters

The first digit is the part, so a number says where a chapter belongs: `0x` is
what everyone needs, `1x` publishes, `2x` is toolchains and targets, `3x`
extends the build graph, `4x` is devices and accelerators, `5x` is what a
program may parse, `9x` is mcpp itself. Within a part the order is a reading
order, not an alphabet.

### 0x — Everyone

- [00 — How mcpp Works](00-how-mcpp-works.md) — the model every other chapter assumes
- [01 — Getting Started](01-getting-started.md) — install, create, build, run
- [02 — Examples](02-examples.md) — which example teaches what
- [03 — The mcpp.toml Manifest](03-mcpp-toml.md) — what a manifest may say
- [04 — Dependencies and Resolution](04-dependencies.md) — where a dependency comes from, and which version wins
- [05 — Features and Capabilities](05-features-and-capabilities.md) — making part of a package optional
- [06 — Workspaces](06-workspace.md) — several packages, one build
- [07 — Testing](07-testing.md) — including what does not run on this machine
- [08 — Commands by Scenario](08-commands-by-scenario.md) — the lookup, once the nouns are known

### 1x — Publishing

- [10 — Packaging an Application for Release](10-pack-and-release.md)
- [11 — Publishing a Library to mcpp-index](11-publishing-a-library.md)
- [12 — Distributing a Prebuilt Library](12-binary-distribution.md)

### 2x — Toolchains and targets

- [20 — Toolchain Management](20-toolchains.md)
- [21 — The Target Triple](21-the-target-triple.md)
- [22 — The Target Side](22-target-side.md)
- [23 — The Project Environment](23-the-project-environment.md)
- [24 — Cross-Compilation Over openkal](24-openkal-cross.md)

### 3x — Extending the build graph

- [30 — Build Programs: `build.mcpp`](30-build-mcpp.md) — a project that needs a step mcpp has no rule for
- [31 — Authoring a Rule Package](31-authoring-a-rule-package.md) — packaging that step for other projects

### 4x — Devices and accelerators

- [40 — Bare-Metal and Freestanding Targets](40-baremetal.md)
- [41 — Reaching a Device](41-devices.md)
- [42 — Heterogeneous Builds](42-heterogeneous-builds.md)

### 5x — Contracts for programs

- [50 — Machine-Readable Output](50-machine-output.md)
- [51 — Supported Versions and Compatibility](51-supported-versions.md)

### 9x — mcpp itself

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
