---
name: mcpp-freestanding
description: Build bare-metal and freestanding mcpp targets using BSP-provided startup, ABI/runtime layers, link scripts and an actual execution runner.
---

# Freestanding Targets

## Start from a working board shape

Use a supplied project template or a verified BSP example rather than deleting
hosted libraries from a normal command line:

```sh
mcpp new firmware --template riscv-virt-rt
cd firmware
mcpp build
```

Confirm the template package is available in the configured index. Its toolchain,
BSP and emulator are prerequisites; source inspection does not verify a board.

## State the target world

A freestanding target has an `os = none` model. The BSP/platform supplies the
required compiler runtime, C environment/ABI or its absence, startup object,
entry symbol, memory/link script and runner. These are contracts, not one
universal `-nostdlib` recipe.

Product `start` states the image entry symbol. A BSP can contribute `link_start`
and `link_script`; disagreement between entry declarations must be resolved,
not hidden with another linker flag. Use an entry target / main-from when a
package supplies reusable startup code.

## Standard-library expectations

Do not assume the hosted `import std` implementation exists for every bare-metal
row. Check the selected freestanding library surface and available compiler
runtime. Building the host-side generator still uses a host graph.

## Validate compile and execution separately

Use `test --no-run` for compile-only checks. Use a real runner/emulator for
execution, preserving status and expected output. Flashing successfully does
not prove the firmware reached its entry or passed a test. Record board model,
ISA, toolchain and runner for reproducibility.

## References

- [40-baremetal.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/40-baremetal.md)
- [34-authoring-a-bsp.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/34-authoring-a-bsp.md)
- [41-devices.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/41-devices.md)
- [specs/target-side.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/target-side.md)
