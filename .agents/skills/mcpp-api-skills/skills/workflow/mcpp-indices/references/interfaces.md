# Indices Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp index` | command | Manage configured package registries |
| `mcpp index add` | command | Add a custom registry |
| `mcpp index add <name>` | required argument | Registry name |
| `mcpp index add <url>` | required argument | Registry URL |
| `mcpp index list` | command | List configured registries |
| `mcpp index list --format` | value | json: the mcpp.index envelope |
| `mcpp index pin` | command | Pin a custom index to a commit rev in mcpp.toml |
| `mcpp index pin <name>` | required argument | Index name |
| `mcpp index pin <rev>` | optional argument | Commit sha (defaults to current lock rev) |
| `mcpp index remove` | command | Remove a registry |
| `mcpp index remove <name>` | required argument | Registry name |
| `mcpp index status` | command | Show local index presence/freshness (offline) |
| `mcpp index unpin` | command | Remove rev pin from a custom index in mcpp.toml |
| `mcpp index unpin <name>` | required argument | Index name |
| `mcpp index update` | command | Refresh local registry clones (global repos always sync in full) |
| `mcpp index update <name>` | optional argument | Update only this PROJECT-level custom index (the global repos sync regardless) |

## Manifest fields

| Interface | Details |
| --- | --- |
| `indices` | Table. One value for the build graph. Merged by key. May be shared through the workspace. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
