# User Documentation

**English** | [简体中文](zh/README.md)

This tree is the **usage manual for what mcpp implements**. Each chapter states
what a capability does, how it is written, and what its current limits are. The
reasoning behind a design, the alternatives that were rejected, and work that is
planned rather than shipped are deliberately absent — they belong to the design
records, which are not user documentation.

## Where a document lives

| tree | reader | contents |
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
| write a program | [01](01-getting-started.md), [04](04-mcpp-toml.md) §1 | [`01-hello`](../examples/01-hello/), [`02-with-deps`](../examples/02-with-deps/) |
| write a library others import | [11](11-publishing-a-library.md), [06](06-features-and-capabilities.md), [04](04-mcpp-toml.md) §2.4 | [`04-workspace`](../examples/04-workspace/), [`11-features`](../examples/11-features/) |
| publish it | [10](10-pack-and-release.md), [11](11-publishing-a-library.md), [12](12-binary-distribution.md) | [`03-pack-static`](../examples/03-pack-static/), [`05-lib-distribution`](../examples/05-lib-distribution/) |
| build for another machine | [21](21-the-target-triple.md), [24](24-openkal-cross.md), [40](40-baremetal.md) | [`06-openkal-cross`](../examples/06-openkal-cross/), `mcpp new … --template riscv-virt-rt` |
| use a GPU or an accelerator | [42](42-heterogeneous-builds.md), [41](41-devices.md) | [`09-heterogeneous`](../examples/09-heterogeneous/), starting at [`boundary/`](../examples/09-heterogeneous/boundary/) |
| add a rule, a language or a generator | [31](31-authoring-a-rule-package.md), [30](30-build-mcpp.md) | [`08-build-rules`](../examples/08-build-rules/), [`12-a-new-device-language`](../examples/12-a-new-device-language/) |
| add a package to the index | [11](11-publishing-a-library.md), [SPEC-001](specs/package-identity.md) | [09](09-commands-by-scenario.md) — the publishing scenarios |
| package a tool, a driver or a board for others | [32](32-authoring-a-payload.md), [33](33-authoring-an-adapter.md), [34](34-authoring-a-bsp.md) | the descriptors in `xim-pkgindex` and `mcpp-index` |
| change mcpp itself | [90](90-build-from-source.md), [92](92-release.md), [51](51-supported-versions.md) | — |

Lessons also arrive as project templates, which a package ships and `mcpp new
--template` instantiates. `riscv-virt-rt` (bare metal) and `ocornut.imgui`
(a graphical application) are the two documented today; the chapter that uses
one names it.

## Chapters

The first digit is the part, so a number says where a chapter belongs:

| | |
|---|---|
| `0x` | the fundamentals |
| `1x` | publishing |
| `2x` | toolchains and targets |
| `3x` | extending mcpp and its ecosystem |
| `4x` | devices and accelerators |
| `5x` | the contracts a program may parse |
| `9x` | mcpp itself |

Within a part the order is a reading order, not an alphabet.

### 0x — Fundamentals

- [00 — What mcpp Is](00-what-mcpp-is.md) — what it is, what it does, and one session that has been run
- [01 — Getting Started](01-getting-started.md) — install, create, build, run
- [02 — Scenarios](02-scenarios.md) — what mcpp is used for, and which features each kind of work uses
- [03 — Examples](03-examples.md) — which example teaches what
- [04 — The mcpp.toml Manifest](04-mcpp-toml.md) — what a manifest may say
- [05 — Dependencies and Resolution](05-dependencies.md) — where a dependency comes from, and which version wins
- [06 — Features and Capabilities](06-features-and-capabilities.md) — making part of a package optional
- [07 — Workspaces](07-workspace.md) — several packages, one build
- [08 — Testing](08-testing.md) — including what does not run on this machine
- [09 — Commands by Scenario](09-commands-by-scenario.md) — the lookup, once the nouns are known

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

### 3x — Extending mcpp and its ecosystem

- [30 — Build Programs: `build.mcpp`](30-build-mcpp.md) — a project that needs a step mcpp has no rule for
- [31 — Authoring a Rule Package](31-authoring-a-rule-package.md) — packaging that step for other projects
- [32 — Authoring a Payload](32-authoring-a-payload.md) — a tool or a prebuilt library mcpp installs
- [33 — Authoring a Runtime Adapter](33-authoring-an-adapter.md) — reaching a library the host supplies
- [34 — Authoring a Board-Support Package](34-authoring-a-bsp.md) — a board, and the way in

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

## Look it up

The chapter list above is a **reading order**. This is the other index: from a
token in front of a reader to the chapter that owns it.

**Manifest tables and keys**

| | chapter | | chapter |
|---|---|---|---|
| `[package]`, `[targets.<n>]`, `[build]`, `[lib]` | [04](04-mcpp-toml.md) | `[profile.<n>]`, `[resources]`, `[runtime]` | [04](04-mcpp-toml.md) |
| `[dependencies]`, `[dev-dependencies]`, `[build-dependencies]` | [05](05-dependencies.md) | `scan_overrides`, `module_extensions` | [04](04-mcpp-toml.md) |
| `[features]`, `[feature-deps.<f>]`, `provides` / `requires` | [06](06-features-and-capabilities.md) | `[workspace]` | [07](07-workspace.md) |
| `[toolchain]`, `cxx_runtime` | [20](20-toolchains.md) | `[target.<sel>]`, `cfg(…)` | [22](22-target-side.md) |
| `[xlings]`, `[xlings.workspace]`, `[feature-xlings.<f>]` | [23](23-the-project-environment.md) | `[pack]` | [10](10-pack-and-release.md) |
| `[build] accel`, `[package] accelerators`, `device_extensions` | [42](42-heterogeneous-builds.md) | `[hooks]` | [09](09-commands-by-scenario.md) |
| `[package] platforms`, `[build] cache` | [04](04-mcpp-toml.md) | `[targets.<name>]`, `[profile.<name>]` | [04](04-mcpp-toml.md) |
| `runner`, `[target.<t>.runners]` | [41](41-devices.md) | `rule_module` | [31](31-authoring-a-rule-package.md) |

**Commands**

| | chapter | | chapter |
|---|---|---|---|
| `build`, `run` | [01](01-getting-started.md) | `test` | [08](08-testing.md) |
| `new`, `new --template` | [01](01-getting-started.md) | `add`, `update`, `why` | [05](05-dependencies.md) |
| `pack` | [10](10-pack-and-release.md) | `publish`, `emit xpkg`, `xpkg parse` | [11](11-publishing-a-library.md) |
| `toolchain` | [20](20-toolchains.md) | `clean`, `cache`, `index`, `self …` | [09](09-commands-by-scenario.md) |

**Concepts**

| | chapter | | chapter |
|---|---|---|---|
| what mcpp is, and the guarantee | [00](00-what-mcpp-is.md) | `import std`, module interfaces, BMIs | [20](20-toolchains.md), [30](30-build-mcpp.md) |
| `mcpp::action`, a build program | [30](30-build-mcpp.md) | a rule package | [31](31-authoring-a-rule-package.md) |
| a target triple, the support matrix | [21](21-the-target-triple.md) | a runner, a named runner | [41](41-devices.md) |
| an island, a seam, `accel`, `MCPP_EXPORT_C` | [42](42-heterogeneous-builds.md) | a descriptor, an index | [11](11-publishing-a-library.md) |
| an ABI tag, a prebuilt artifact | [12](12-binary-distribution.md) | exit codes, JSON output | [50](50-machine-output.md) |
| an `xim:` payload, `[xlings.workspace]` | [32](32-authoring-a-payload.md), [23](23-the-project-environment.md) | a `compat:` adapter, `runtime.library_dirs` | [33](33-authoring-an-adapter.md) |

## Specifications

Normative documents — semantics, constraints and matching rules, each rule
tagged with its implementation status. For index authors, contributors and
downstream tooling.

- [specs/](specs/README.md) — index of all specs
  - [SPEC-001 — Package identity, `[dependencies]` selectors and matching](specs/package-identity.md)
  - [SPEC-002 — The target side: reserved namespace, five layers, three rules](specs/target-side.md)
  - [SPEC-003 — The exit-code contract](specs/exit-codes.md)
  - [SPEC-004 — `mcpp.toml` planes, conditioning shape, resolution axes and naming](specs/manifest-semantics.md)
