# Toolchains Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp toolchain` | command | Install / list / select / remove C++ toolchains |
| `mcpp toolchain default` | command | Set the default toolchain (and optionally the default target) |
| `mcpp toolchain default --target` | value | Default build target <triple> (omit = host) |
| `mcpp toolchain default <spec>` | required argument | <family>[@<version>] (version may be partial) |
| `mcpp toolchain default <version>` | optional argument | (optional, alternative to @-form) |
| `mcpp toolchain install` | command | Install a toolchain via mcpp's xlings |
| `mcpp toolchain install --platform` | value | With --target: the target's platform for this invocation (= MCPP_PLATFORM) |
| `mcpp toolchain install --target` | value | Install the toolchain payload for <triple> (e.g. x86_64-windows-gnu) |
| `mcpp toolchain install <compiler>` | optional argument | gcc \| llvm \| msvc \| emsdk \| android-ndk (or gcc@16.1.0; legacy aliases accepted) |
| `mcpp toolchain install <version>` | optional argument | e.g. 16.1.0, 15, 15.1 |
| `mcpp toolchain list` | command | List installed toolchains |
| `mcpp toolchain list --format` | value | Machine-readable output (enveloped; see docs/50-machine-output.md) |
| `mcpp toolchain remove` | command | Uninstall a toolchain |
| `mcpp toolchain remove --target` | value | Remove the payload for <triple> instead of the host one |
| `mcpp toolchain remove <spec>` | required argument | <family>[@<version>] (exact version) |
| `mcpp toolchain remove <version>` | optional argument | (optional, alternative to @-form) |

## Manifest fields

| Interface | Details |
| --- | --- |
| `toolchain` | Table. One value for the build graph. Merged by key. May be shared through the workspace. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
