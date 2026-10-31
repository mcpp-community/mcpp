# Features Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Manifest fields

| Interface | Details |
| --- | --- |
| `feature-deps` | Table. |
| `features` | Table. Applies to this package's inputs. Merged by key. |
| `features.<name>.defines` | See the configuration examples in SKILL.md. |
| `features.<name>.device_extensions` | See the configuration examples in SKILL.md. |
| `features.<name>.flags` | See the configuration examples in SKILL.md. |
| `features.<name>.forward` | See the configuration examples in SKILL.md. |
| `features.<name>.implies` | See the configuration examples in SKILL.md. |
| `features.<name>.provides` | See the configuration examples in SKILL.md. |
| `features.<name>.requires` | See the configuration examples in SKILL.md. |
| `features.<name>.requires_abi` | See the configuration examples in SKILL.md. |
| `features.<name>.rule_module` | See the configuration examples in SKILL.md. |
| `features.<name>.sources` | See the configuration examples in SKILL.md. |
| `features.<name>.tools` | See the configuration examples in SKILL.md. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
