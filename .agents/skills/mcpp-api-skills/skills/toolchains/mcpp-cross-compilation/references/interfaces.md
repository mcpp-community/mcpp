# Cross Compilation Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Manifest fields

| Interface | Details |
| --- | --- |
| `host` | Table. One value for the build graph. Merged by key. |
| `platform` | Table. Applies to this package's inputs. Merged by key. |
| `target.<sel>.abi` | One value for the build graph. Merged by key. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.carrier` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.cxx_runtime` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.dependencies` | Applies to this package's inputs. Merged by key. Supports conditional target settings. |
| `target.<sel>.layers` | One value for the build graph. Merged by key. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.link` | One value for the build graph. Merged by key. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.linkage` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.min_api_level` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.platform` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.run_under` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.runner` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.runtime` | Propagates to consumers. Merged by key. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.sysroot` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.targets` | Applies to this package's inputs. Merged by key. Supports conditional target settings. |
| `target.<sel>.toolchain` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.tools` | One value for the build graph. Merged by key. May be shared through the workspace. Supports conditional target settings. |
| `target.<sel>.xlings` | One value for the build graph. Merged by key. May be shared through the workspace. Supports conditional target settings. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
