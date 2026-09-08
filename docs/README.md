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
| write a program | [00](00-getting-started.md), [05](05-mcpp-toml.md) §1 | [`01-hello`](../examples/01-hello/), [`02-with-deps`](../examples/02-with-deps/) |
| write a library others import | [10](10-publishing-a-library.md), [22](22-features-and-capabilities.md), [05](05-mcpp-toml.md) §2.4 | [`04-workspace`](../examples/04-workspace/), [`11-features`](../examples/11-features/) |
| publish it | [02](02-pack-and-release.md), [10](10-publishing-a-library.md), [12](12-binary-distribution.md) | [`03-pack-static`](../examples/03-pack-static/), [`05-lib-distribution`](../examples/05-lib-distribution/) |
| build for another machine | [16](16-the-target-triple.md), [15](15-openkal-cross.md), [13](13-baremetal.md) | [`06-openkal-cross`](../examples/06-openkal-cross/), `mcpp new … --template riscv-virt-rt` |
| use a GPU or an accelerator | [20](20-heterogeneous-builds.md), [18](18-devices.md) | [`09-heterogeneous`](../examples/09-heterogeneous/), starting at [`boundary/`](../examples/09-heterogeneous/boundary/) |
| add a rule, a language or a generator | [23](23-authoring-a-rule-package.md), [07](07-build-mcpp.md) | [`08-build-rules`](../examples/08-build-rules/), [`12-a-new-device-language`](../examples/12-a-new-device-language/) |
| add a package to the index | [10](10-publishing-a-library.md), [SPEC-001](specs/package-identity.md) | [21](21-commands-by-scenario.md) — the publishing scenarios |
| change mcpp itself | [04](04-build-from-source.md), [09](09-release.md), [19](19-supported-versions.md) | — |

Lessons also arrive as project templates, which a package ships and `mcpp new
--template` instantiates. `riscv-virt-rt` (bare metal) and `ocornut.imgui`
(a graphical application) are the two documented today; the chapter that uses
one names it.

## Chapters

### Part I — Using mcpp

- [00 - Getting Started](00-getting-started.md)
- [01 - Examples](01-examples.md)
- [05 - mcpp.toml Manifest Guide](05-mcpp-toml.md)
- [06 - Workspaces](06-workspace.md)
- [07 - build.mcpp Build Program](07-build-mcpp.md)
- [21 - Commands by Scenario](21-commands-by-scenario.md)
- [22 - Features and Capabilities](22-features-and-capabilities.md)

### Part II — Shipping what was built

- [02 - Packaging & Release](02-pack-and-release.md)
- [10 - Publishing a Library to mcpp-index](10-publishing-a-library.md)
- [12 - Distributing a Prebuilt Library](12-binary-distribution.md)

### Part III — Toolchains and targets

- [03 - Toolchain Management](03-toolchains.md)
- [13 - Bare-Metal and Freestanding Targets](13-baremetal.md)
- [14 - The Target Side](14-target-side.md)
- [15 - Cross-Compilation Over openkal](15-openkal-cross.md)
- [16 - The Target Triple](16-the-target-triple.md)
- [17 - The Project Environment](17-the-project-environment.md)

### Part IV — Devices and accelerators

- [18 - Reaching a Device](18-devices.md)
- [20 - Heterogeneous Builds](20-heterogeneous-builds.md)

### Part V — Extending mcpp from outside

- [23 - Authoring a Rule Package](23-authoring-a-rule-package.md)

### Part VI — Machine interfaces and compatibility

- [11 - Machine-Readable Output](11-machine-output.md)
- [19 - Supported Versions and Compatibility](19-supported-versions.md)

### Part VII — Contributing to mcpp itself

- [04 - Building from Source & Contributing](04-build-from-source.md)
- [08 - Toolchain Internals](08-toolchain-internals.md)
- [09 - Releasing mcpp](09-release.md)

## Specifications

Normative documents — semantics, constraints and matching rules, each rule
tagged with its implementation status. For index authors, contributors and
downstream tooling.

- [specs/](specs/README.md) — index of all specs
  - [SPEC-001 — Package identity, `[dependencies]` selectors and matching](specs/package-identity.md)
  - [SPEC-002 — The target side: reserved namespace, five layers, three rules](specs/target-side.md)
  - [SPEC-003 — The exit-code contract](specs/exit-codes.md)
  - [SPEC-004 — `mcpp.toml` planes, conditioning shape, resolution axes and naming](specs/manifest-semantics.md)
