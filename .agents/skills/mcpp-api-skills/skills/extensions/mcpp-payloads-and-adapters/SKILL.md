---
name: mcpp-payloads-and-adapters
description: Package prebuilt tools and libraries, runtime adapters, BSP contributions and custom distribution formats for mcpp consumers.
---

# Payloads and Adapters

## Choose the package role

- A source library is built by mcpp: use `mcpp-publishing`.
- A payload is installed, not compiled by mcpp: a tool, driver, SDK or library.
- A runtime adapter makes a host-supplied library reachable under a declared ABI.
- A BSP supplies startup/link/runtime/device facts for a board.
- A format provider turns a staged tree into a distributable artifact.

These roles can share build-program primitives without sharing a descriptor
schema. Do not paste a source-library descriptor into a tool payload recipe.

## Payload workflow

Use the xpkg/xim package descriptor and hooks to place files in the install root
and register offered programs/libraries. Declare actual architectures, systems,
release URLs, checksums, license and dependencies. A `latest` reference is not
a reproducible release pin. Consumer access goes through declared workspace or
feature-xlings requirements, not an unrelated global install.

Installation hooks can receive target information before the toolchain is fully
resolved. They must not populate one version-keyed shared store directory with
an unkeyed target-specific build variant. Empty compiler/library variables are
not permission to guess the target ABI.

## Runtime adapters and BSPs

Adapters declare requirements and measured host facts, locate the required
runtime files and contribute correct search/deploy information. Do not claim
a provider works merely because the host has a similarly named `.so` or DLL.
BSPs additionally supply the target ABI/runtime, startup entry, linker script
and runner as needed; validation requires a real or emulated board.

## Custom package formats

Declare `provides_pack_format` unconditionally so discovery works. Submit
format work only when that format is requested. Use artifact actions and the
stage/target handles; report the actual artifact. Do not reserve an engine
format name or assume every format is available in every resolved graph.

Validate installation on a fresh isolated home and consumption by another
project. Recipe syntax alone does not prove a usable payload or ABI.

## References

- [32-authoring-a-payload.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/32-authoring-a-payload.md)
- [33-authoring-an-adapter.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/33-authoring-an-adapter.md)
- [34-authoring-a-bsp.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/34-authoring-a-bsp.md)
- [specs/build-plugins.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/build-plugins.md)
