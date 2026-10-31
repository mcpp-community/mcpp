# Publishing Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp emit` | command | Generate a document describing this project (xpkg, sbom, build-database) |
| `mcpp emit sbom` | command | Write a CycloneDX bill of materials for the recorded resolution |
| `mcpp emit sbom --output` (`-o`) | value | Write to file instead of stdout |
| `mcpp emit xpkg` | command | Generate xpkg Lua entry |
| `mcpp emit xpkg --namespace` | value | Package namespace for the emitted descriptor (overrides [package] namespace). Emits both `namespace` and the fully-qualified `name` |
| `mcpp emit xpkg --output` (`-o`) | value | Write to file instead of stdout |
| `mcpp emit xpkg --package` | value; repeatable | Describe packages `mcpp pack` wrote (repeatable): an xim-pkgindex entry of the program, from their mcpp-package.json |
| `mcpp emit xpkg --url` | value | With --package: where the packages are published (<PREFIX>/<file>) |
| `mcpp emit xpkg --version` (`-V`) | value | Override package version |
| `mcpp publish` | command | Publish package to default registry |
| `mcpp publish --allow-dirty` | flag | Allow uncommitted changes |
| `mcpp publish --dry-run` | flag | Print xpkg.lua without uploading |
| `mcpp xpkg` | command | Inspect / validate xpkg descriptors |
| `mcpp xpkg parse` | command | Parse a descriptor's mcpp segment exactly as the resolver would (strict: unknown keys are errors) |
| `mcpp xpkg parse --all-os` | flag | Validate every per-OS section (linux/macosx/windows), not just the running host's |
| `mcpp xpkg parse --allow-split-name` | flag | OBSOLETE (kept accepted so 0.0.105-era index CI keeps working): skip the package.name form check. Since 0.0.106 the canonical form IS the short name, so xlings-native descriptors pass without this flag |
| `mcpp xpkg parse --allow-unknown` | flag | Downgrade unknown mcpp-segment keys from error to warning |
| `mcpp xpkg parse --format` | value | Machine-readable output (enveloped; see docs/50-machine-output.md) |
| `mcpp xpkg parse --json` | flag | Emit machine-readable JSON (legacy payload, kept for ever) |

## Source

- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
