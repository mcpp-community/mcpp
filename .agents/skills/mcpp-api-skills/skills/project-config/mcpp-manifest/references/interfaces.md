# Manifest Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Manifest fields

| Interface | Details |
| --- | --- |
| `capabilities` | Table. |
| `feature-xlings` | Table. |
| `generated_files` | Table. |
| `hooks` | Table. Applies to this package's inputs. Merged by key. |
| `package` | Table. Describes the package. Merged by key. May be shared through the workspace. |
| `package.accelerators` | Array of strings. |
| `package.authors` | Array of strings. |
| `package.description` | String. |
| `package.license` | String. |
| `package.mcpp` | String. |
| `package.metadata` | Table. |
| `package.name` | String. |
| `package.namespace` | String. |
| `package.platforms` | Array of strings. |
| `package.repo` | String. |
| `package.standard` | String. Integer. |
| `package.std-compat-module` | String. |
| `package.std-module` | String. |
| `package.std-module-flags` | Array of strings. |
| `package.version` | String. |
| `scan_overrides` | Table. |
| `target` | Table. |
| `tools.overrides` | Table. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
