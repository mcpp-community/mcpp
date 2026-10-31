# Targets Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Manifest fields

| Interface | Details |
| --- | --- |
| `lib` | Table. |
| `lib.path` | String. |
| `resources` | Table. Applies to this package's inputs. Merged by key. |
| `resources.extra-inputs` | Array of strings. |
| `resources.files` | Array of strings. |
| `resources.icon` | String. |
| `resources.version-info` | See the configuration examples in SKILL.md. |
| `resources.version-info.company` | See the configuration examples in SKILL.md. |
| `resources.version-info.copyright` | See the configuration examples in SKILL.md. |
| `resources.version-info.description` | See the configuration examples in SKILL.md. |
| `resources.version-info.internal-name` | See the configuration examples in SKILL.md. |
| `resources.version-info.original-filename` | See the configuration examples in SKILL.md. |
| `resources.version-info.product` | See the configuration examples in SKILL.md. |
| `targets` | Table. Applies to this package's inputs. Merged by key. |
| `targets.<name>.cflags` | See the configuration examples in SKILL.md. |
| `targets.<name>.cxxflags` | See the configuration examples in SKILL.md. |
| `targets.<name>.defines` | See the configuration examples in SKILL.md. |
| `targets.<name>.exports` | See the configuration examples in SKILL.md. |
| `targets.<name>.kind` | See the configuration examples in SKILL.md. |
| `targets.<name>.linkage` | See the configuration examples in SKILL.md. |
| `targets.<name>.main` | See the configuration examples in SKILL.md. |
| `targets.<name>.required_features` | See the configuration examples in SKILL.md. |
| `targets.<name>.soname` | See the configuration examples in SKILL.md. |
| `targets.<name>.start` | See the configuration examples in SKILL.md. |
| `targets.<name>.windows_auto_export` | See the configuration examples in SKILL.md. |
| `targets.<name>.windows_code_page` | See the configuration examples in SKILL.md. |
| `targets.<name>.windows_entry` | See the configuration examples in SKILL.md. |
| `targets.<name>.windows_long_paths` | See the configuration examples in SKILL.md. |
| `targets.<name>.windows_subsystem` | See the configuration examples in SKILL.md. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
