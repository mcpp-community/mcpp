# Troubleshooting Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp explain` | command | Show extended description for an error code |
| `mcpp explain <code>` | required argument | Error code such as E0001, or a refusal's reason token such as platform-version-group |
| `mcpp why` | command | Explain how the toolchain / runtime / deps / runners / sources were resolved |
| `mcpp why --features` | value | Resolve with these features, as `build --features` does (root features and <dependency>/<feature>) |
| `mcpp why --format` | value | Machine-readable output (enveloped; see docs/50-machine-output.md) |
| `mcpp why --host-platform` | value | The host graph's platform for this invocation (= MCPP_HOST_PLATFORM) |
| `mcpp why --platform` | value | The target's platform for this invocation (= MCPP_PLATFORM): <name>@<version>, path:<dir>, none |
| `mcpp why --sysroot` | value | The target's base for this invocation (= MCPP_SYSROOT): a package, glibc@system, path:<dir>, none |
| `mcpp why --target` | value | Ask about this target instead of the project's default |
| `mcpp why --toolchain` | value | Ask about this toolchain, e.g. llvm@22.1.8 |
| `mcpp why <subject>` | optional argument | for `tool` and `payload`: the name to look up |
| `mcpp why <topic>` | optional argument | toolchain \| sysroot \| runtime \| deps \| runners \| sources \| tool \| payload \| build-programs \| compat (default: all) |

## Source

- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
