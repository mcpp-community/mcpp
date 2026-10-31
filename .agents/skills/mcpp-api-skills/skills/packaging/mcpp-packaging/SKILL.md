---
name: mcpp-packaging
description: Package mcpp applications and libraries with explicit linkage, closure, format, target legs, compression, debug symbols and machine-readable artifact results.
---

# Packaging Deliverables

## Inspect and package

```sh
mcpp pack --list-formats
mcpp pack --pkg-format dir
mcpp pack --pkg-format tar --message-format json
```

`pack <product>` selects a product, not a triple. A program becomes a bundle;
a library becomes interface plus prebuilt binaries. No product name is safe
only when the project has an unambiguous packable product.

## Keep the axes independent

| Axis | Selection |
|---|---|
| Libraries linked inside/beside the program | `--linkage static|shared` |
| What the receiving machine provides | `--mode system|vendored|self-contained` |
| Container | `--pkg-format` |
| Target legs | Repeated `--target`, otherwise pack/build target settings |
| Carrier systems | `--systems` |

`--mode static` is shorthand for static linkage plus self-contained closure.
On Linux a static linkage selection uses the applicable musl row. A container
extension does not establish runtime independence.

Built-in formats include tar/zip/dir and supported single-file/polyglot shapes;
resolved packages can add formats. Discover instead of hard-coding a universal
format list. Comma-separated containers can share one staged tree. Use an output
directory for a command producing several files.

## Manifest and advanced options

`[pack]` supplies defaults and bundle policy. CLI options include compression,
debug-symbol extraction, strip policy and merging packages the command wrote.
Use `--pkg-format`; the earlier `--format` spelling is retained with compatibility
handling. `--message-format json` is unrelated to the container choice.

## Verify what ships

Read the `mcpp.pack` result's artifacts and staged tree; do not infer filenames
from an old naming convention. Run from the package on the intended receiving
system, and check for build-machine paths and missing dynamic libraries.
Validate each target leg and carrier system separately. A successful development
`mcpp run` is not a deployment test.

## References

[Interface reference](references/interfaces.md).

- [10-pack-and-release.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/10-pack-and-release.md)
- [12-binary-distribution.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/12-binary-distribution.md)
- [specs/packaging.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/packaging.md)
