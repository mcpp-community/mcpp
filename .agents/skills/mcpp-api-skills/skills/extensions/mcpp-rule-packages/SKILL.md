---
name: mcpp-rule-packages
description: Author and consume reusable mcpp rule modules, feature-gated device extensions and host-built tools, including explicit build-time reexports.
---

# Rule Packages and Host Graphs

## Package the task, not an engine patch

A rule package is an ordinary package exporting a C++ module that submits
actions. A feature can declare `rule_module` and `device_extensions`. The
consumer activates the feature and imports the rule module in its build program.

```toml
[features]
default = []
shader = { rule_module = "myrules.shader", device_extensions = [".shaderx"] }
```

The package must actually export that module and implement its callable API.
Do not invent a vendor-specific engine rule name.

## Host versus target libraries

Build-program libraries belong in `[build-dependencies]`; host tools can be
requested with a dependency edge's `tools = [...]`. Resolve them with `dep_bin`
rather than searching target output directories. `host-module` and rule-module
imports contribute to the host graph.

Ordinary target dependencies can still expose a rule through their declared
feature contract. Do not assume every target module is available to the build
program merely because the product imports it.

## Reexports

Build-time tools, host modules and dependency directories do not automatically
flow through every dependency edge. Set `reexport = true` only where the package
intends to expose them, and test a transitive consumer. This is separate from
public target include/link usage.

## Language extension limits

A rule can claim new device suffixes. It cannot reclassify `.cpp`, `.cppm`,
`.c` or another built-in source role. Module-interface suffixes belong to the
project's module configuration. Use source/object/artifact roles based on the
actual output, and declare generated module identities.

Test the rule from a separate consumer with only its declared dependencies.
A successful root workspace build may hide an undeclared host dependency.

## References

- [31-authoring-a-rule-package.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/31-authoring-a-rule-package.md)
- [specs/build-plugins.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/build-plugins.md)
