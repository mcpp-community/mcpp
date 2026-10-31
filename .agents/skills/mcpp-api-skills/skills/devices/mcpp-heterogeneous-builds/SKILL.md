---
name: mcpp-heterogeneous-builds
description: Configure accelerator selections and rule-driven device languages in mcpp, preserving host/device ABI boundaries and honest backend validation.
---

# Heterogeneous Builds

## Select the variant

```sh
mcpp build --accel 'cuda12.8+{sm_80}'
mcpp build --no-accel
```

The accelerator spelling and architecture must be supported by the actual
provider/toolkit; this example is not a device availability assertion. Inspect
the package's rules and available toolchain before choosing a backend.

`[build] accel` and CLI selection describe the device variant. Package
`accelerators` and feature-gated tools describe offered/required capabilities.
Feature activation alone does not prove a device backend was compiled.

## Keep the seam explicit

Device code is compiled by rule actions. The C++ module graph stays on the host
side; use an explicit C ABI boundary where a vendor compiler cannot participate
in that module graph. Do not pass C++ standard-library objects or compiler-
specific layout across the seam without an established ABI contract.

Generated C++ interfaces use source actions and declared module identities.
Device objects/binaries use the role matching their actual output. Toolkits and
large dependencies must be gated by the backend that needs them.

## Add a device language

A rule feature declares `device_extensions` and `rule_module`, submits actions
and declares its compiler as an input. It cannot take over built-in C/C++
extensions. A source suffix recognized as device code is not evidence that a
compiler rule actually handles it.

## Validate in layers

First validate the host/device boundary without a physical device. Then verify
toolkit installation and compile. Finally execute on the intended device and
check results. Report these stages separately; do not claim a runtime backend
from a successful import, parse, cached object or skipped capability test.

## References

- [42-heterogeneous-builds.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/42-heterogeneous-builds.md)
- [31-authoring-a-rule-package.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/31-authoring-a-rule-package.md)
