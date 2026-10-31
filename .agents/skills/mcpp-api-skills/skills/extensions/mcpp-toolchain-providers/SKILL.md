---
name: mcpp-toolchain-providers
description: Configure local or program-provided mcpp toolchains using bootstrap/configure phases, tool roles and explicit origins rather than hard-coded compiler layouts.
---

# Toolchain Providers

## Choose the smallest extension

Use a normal toolchain selector for an existing payload. A local toolchain can
declare path/prefix/family and role-specific programs. Use a configure program
only when locating or selecting the toolchain requires computation.

```toml
[toolchain]
default = { configure = "build.mcpp" }
```

Each `[toolchain]` entry selects a toolchain for `default` or a host platform
such as `windows`. A configure entry is a table containing only
`configure = "build.mcpp"`; do not combine it with `path`, `family` or `tools`.
The separate `bootstrap` entry names the toolchain used to build host programs.

The root program runs in the toolchain phase before ordinary configuration.
Its bootstrap toolchain must be available before it can discover the product
toolchain. Use `phase()` to select the statements for each invocation.

## Emit declared toolchain facts

The public `toolchain(key,value)` API accepts the configured toolchain keys,
including spec/path/prefix/sysroot/family/launcher, `tool.<role>` and origin.
It is not valid in an arbitrary ordinary or platform invocation. Use `phase()`
to distinguish the runs; non-toolchain work belongs in the later appropriate
phase rather than being unconditionally executed twice.

Use `tool(role)` and `abi_tool(role)` in downstream rules instead of deriving
`ar`, `ld` or dlltool from a guessed compiler sibling path. An unavailable role
must produce a prerequisite diagnostic, not a bare command-not-found error.

## Provenance and incremental inputs

Declare files/environment that change the selected toolchain. When a payload
is optional, follow the request/pending/retry contract before configuring it.
Report actual origins with the public decision channel; do not label a local
PATH program as a registry payload.

## Validate

With the bootstrap prerequisites available, verify that the program actually
runs in the toolchain phase and that its declarations select the intended
compiler. A manifest loading successfully is not evidence that a configure
entry was selected. Inspect `why toolchain`, `why sources` and the fingerprint.

Test native and cross cases separately, including the driver, ABI, sysroot,
linker and host compilation of the configure program. A directory containing
a compiler does not establish a complete C++ modules toolchain.

## References

- [20-toolchains.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/20-toolchains.md)
- [30-build-mcpp.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/30-build-mcpp.md)
- [specs/toolchain-management.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/toolchain-management.md)
- [specs/build-plugins.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/build-plugins.md)
