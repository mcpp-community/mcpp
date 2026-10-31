# Runtime Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Manifest fields

| Interface | Details |
| --- | --- |
| `c-abi` | Table. |
| `c-abi.builtins` | See the configuration examples in SKILL.md. |
| `c-abi.data-model` | See the configuration examples in SKILL.md. |
| `c-abi.presents` | See the configuration examples in SKILL.md. |
| `c-abi.wchar` | See the configuration examples in SKILL.md. |
| `kernel-abi` | Table. |
| `kernel-abi.provides-interfaces` | Array of strings. |
| `kernel-abi.requires-interfaces` | Array of strings. |
| `package.c-environment` | String. |
| `package.exclusive` | Array of strings. |
| `package.provides` | Array of strings. |
| `package.requires` | See the configuration examples in SKILL.md. |
| `package.requires_abi` | See the configuration examples in SKILL.md. |
| `runtime` | Table. Applies to this package's inputs. Merged by key. |
| `runtime.artifacts` | See the configuration examples in SKILL.md. |
| `runtime.capabilities` | Array of strings. |
| `runtime.deploy` | See the configuration examples in SKILL.md. |
| `runtime.deploy_files` | See the configuration examples in SKILL.md. |
| `runtime.dlopen_libs` | Array of strings. |
| `runtime.frameworks` | Array of strings. |
| `runtime.libraries` | Array of strings. |
| `runtime.library_dirs` | Array of strings. |
| `runtime.link_library_dirs` | See the configuration examples in SKILL.md. |
| `runtime.provides` | Array of strings. |
| `runtime.requirements` | See the configuration examples in SKILL.md. |
| `runtime.runtime_search_dirs` | See the configuration examples in SKILL.md. |
| `runtime.transitive_needed_dirs` | See the configuration examples in SKILL.md. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
