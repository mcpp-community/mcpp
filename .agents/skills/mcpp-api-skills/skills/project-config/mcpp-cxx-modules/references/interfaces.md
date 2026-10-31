# Cxx Modules Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Manifest fields

| Interface | Details |
| --- | --- |
| `build.module_extensions` | Array of strings. Applies to this package's inputs. Workspace entries precede member entries. |
| `build.sources` | Applies to this package's inputs. Workspace entries precede member entries. Supports conditional target settings. |
| `language` | Table. |
| `language.import_std` | Boolean. |
| `language.modules` | Boolean. |
| `language.standard` | String. |
| `modules.exports` | Array of strings. |
| `modules.sources` | Array of strings. |
| `modules.strict` | Boolean. |

## Source extensions

| Interface | Details |
| --- | --- |
| `.S` | Gas |
| `.asc` | Device |
| `.asm` | Nasm |
| `.c` | C |
| `.cc` | Cxx |
| `.cce` | Device |
| `.cl` | Device |
| `.comp` | Device |
| `.cpp` | Cxx |
| `.cppm` | ModuleInterface |
| `.cu` | Device |
| `.cuh` | DeviceHeader |
| `.cxx` | Cxx |
| `.frag` | Device |
| `.geom` | Device |
| `.glsl` | Device |
| `.h` | Header |
| `.hh` | Header |
| `.hip` | Device |
| `.hiph` | DeviceHeader |
| `.hlsl` | Device |
| `.hpp` | Header |
| `.hxx` | Header |
| `.m` | C |
| `.mesh` | Device |
| `.metal` | Device |
| `.mm` | Cxx |
| `.rahit` | Device |
| `.rcall` | Device |
| `.rchit` | Device |
| `.rgen` | Device |
| `.rint` | Device |
| `.rmiss` | Device |
| `.s` | Gas |
| `.sycl` | Device |
| `.task` | Device |
| `.tesc` | Device |
| `.tese` | Device |
| `.vert` | Device |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
- [modules/source-kind/src/source_kind.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/source-kind/src/source_kind.cppm)
