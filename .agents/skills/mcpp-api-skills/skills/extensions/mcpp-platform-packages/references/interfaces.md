# Platform Packages Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Platform fields

| Interface | Details |
| --- | --- |
| `build` | carrier table |
| `hosts` | carrier table |
| `main` | carrier table |
| `package` | carrier table |
| `section` | carrier table |
| `asmflags` | build table |
| `cflags` | build table |
| `cxxflags` | build table |
| `ldflags` | build table |
| `driver` | link table |
| `exe` | link table |
| `flags` | link table |
| `inputs` | link table |
| `script` | link table |
| `seal` | link table |
| `shared` | link table |
| `start` | link table |
| `carrier` | [target.<selector>] |
| `layers` | [target.<selector>] |
| `link` | [target.<selector>] |
| `platform` | [target.<selector>] |
| `runner` | [target.<selector>] |
| `runners` | [target.<selector>] |
| `tools` | [target.<selector>] |
| `branch` | ref value |
| `default-features` | ref value |
| `features` | ref value |
| `git` | ref value |
| `name` | ref value |
| `path` | ref value |
| `rev` | ref value |
| `tag` | ref value |
| `version` | ref value |
| `cmd` | runner table |
| `exclusive` | runner table |
| `host` | runner table |
| `hosts` | runner table |
| `build` | [platform] |
| `carrier` | [platform] |
| `descriptor_keys` | [platform] |
| `hosted` | [platform] |
| `layers` | [platform] |
| `link` | [platform] |
| `plugin` | [platform] |
| `runner` | [platform] |
| `runners` | [platform] |
| `sysroot` | [platform] |
| `targets` | [platform] |
| `toolchain` | [platform] |
| `tools` | [platform] |
| `universal` | [platform] |
| `versions` | [platform] |

## Source

- [modules/manifest/src/platform_toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/platform_toml.cppm)
