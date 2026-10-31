---
name: mcpp-platform-packages
description: Author and override mcpp platform packages with targets, ABI layers, tools, link contracts, runners, host settings and carriers.
---

# Platform Packages

## Treat platform and machine as separate declarations

A platform package states a class of targets. A project selects it with
`--platform`, a target-row `platform`, a machine configuration or a target-table
default. `--host-platform` selects the build-host graph separately.

Use a package or a local `path:` selector for development; `none` disables a
selection explicitly. A platform does not invent extra pack legs: the command,
pack settings or requested target set determines those.

## Package surface

| Section | Meaning |
|---|---|
| `[platform]` | Targets, toolchain, sysroot, runners and plugin |
| `[platform.layers]` | C ABI, C++ ABI, compiler/kernel runtime packages or false |
| `[platform.tools]` | Programs built for or found on the build machine |
| `[platform.build]` | Graph-wide target compile inputs |
| `[platform.link]` | Driver, flags, script, entry, inputs, artifact names, seal |
| carrier | System-specific launcher/container construction |
| hosted / host | Host-side selection and execution contracts |

Use the local parser reference for accepted shapes. Layer names are contract
identities, not arbitrary compiler flags. A layer supplied by the graph may take
precedence over the platform convention.

## Override one part

```toml
[target.'cfg(platform = "openkal")'.layers]
c-abi = { path = "../openkal-musl" }
```

Use an existing package directory. Compile/link flags append; link inputs and
seal sequences replace whole values; layers/tools/runners replace by key.
Do not assume every override has the same merge behavior. Resolved-layer
conditions cannot decide the dependencies that create the layer.

## Platform program and carriers

Platform-phase statements supply link/artifact/seal/run-host facts through
public APIs. Ordinary package configuration and toolchain statements have
different phase limits. A carrier is a program built for the stated system,
not a shell string glued around a foreign image.

Validate target selection, the actual link/artifact shape, runner reachability
and packaging separately. Do not claim platform support from a parsed table.

## References

[Interface reference](references/interfaces.md).

- [35-platforms.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/35-platforms.md)
- [specs/platforms.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/platforms.md)
- [specs/target-sets.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/target-sets.md)
