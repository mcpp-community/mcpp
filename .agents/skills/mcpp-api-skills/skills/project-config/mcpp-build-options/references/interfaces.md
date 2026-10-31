# Build Options Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Manifest fields

| Interface | Details |
| --- | --- |
| `build` | Table. |
| `build.accel` | String. One value for the build graph. A member declaration overrides the shared value. |
| `build.allow_host_libs` | Boolean. Applies to this package's inputs. A member declaration overrides the shared value. |
| `build.bmi_schedule` | String. One value for the build graph. A member declaration overrides the shared value. |
| `build.build_program_timeout` | Applies to this package's inputs. A member declaration overrides the shared value. |
| `build.c_standard` | String. Applies to this package's inputs. A member declaration overrides the shared value. May be shared through the workspace. |
| `build.cache` | String. One value for the build graph. A member declaration overrides the shared value. |
| `build.cflags` | Array of strings. Applies to this package's inputs. Workspace entries precede member entries. May be shared through the workspace. Supports conditional target settings. |
| `build.compile_commands` | String. Describes the package. A member declaration overrides the shared value. |
| `build.cxx_runtime` | One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. |
| `build.cxxflags` | Array of strings. Applies to this package's inputs. Workspace entries precede member entries. May be shared through the workspace. Supports conditional target settings. |
| `build.default-profile` | String. One value for the build graph. A member declaration overrides the shared value. |
| `build.defines` | Array of strings. Applies to this package's inputs. Member macros override the same names. May be shared through the workspace. Supports conditional target settings. |
| `build.dependency_linkage` | String. One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. |
| `build.dialect_cxxflags` | Array of strings. One value for the build graph. Workspace entries precede member entries. May be shared through the workspace. Supports conditional target settings. |
| `build.flags` | Applies to this package's inputs. Workspace entries precede member entries. Supports conditional target settings. |
| `build.include_dirs` | Array of strings. Propagates to consumers. Workspace entries precede member entries. May be shared through the workspace. Supports conditional target settings. |
| `build.include_dirs_after` | Array of strings. Propagates to consumers. Workspace entries precede member entries. May be shared through the workspace. Supports conditional target settings. |
| `build.ios_deployment_target` | String. One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. |
| `build.jobs` | String. Integer. One value for the build graph. A member declaration overrides the shared value. |
| `build.ldflags` | Array of strings. Propagates to consumers. Workspace entries precede member entries. May be shared through the workspace. Supports conditional target settings. |
| `build.macos_deployment_target` | String. One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. |
| `build.platform-dependencies` | String. One value for the build graph. A member declaration overrides the shared value. |
| `build.private_include_dirs` | Array of strings selecting entries of `include_dirs` that are not propagated to consumers; does not add header search paths. Matching follows glob expansion. May be shared or conditional. |
| `build.profile` | String. One value for the build graph. A member declaration overrides the shared value. |
| `build.static_stdlib` | Boolean. One value for the build graph. A member declaration overrides the shared value. |
| `build.std-compat-module` | String. Applies to this package's inputs. A member declaration overrides the shared value. |
| `build.std-module` | String. Applies to this package's inputs. A member declaration overrides the shared value. |
| `build.std-module-flags` | Array of strings. Applies to this package's inputs. Workspace entries precede member entries. Supports conditional target settings. |
| `build.target` | String. One value for the build graph. A member declaration overrides the shared value. May be shared through the workspace. |
| `profile` | Table. |
| `profile.<name>` | One value for the build graph. Merged by key. May be shared through the workspace. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
