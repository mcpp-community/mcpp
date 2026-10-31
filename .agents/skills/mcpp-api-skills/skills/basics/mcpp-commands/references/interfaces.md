# Commands Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp --deny` | value | Deny named effects; denying global cache writes can select local caching. |
| `mcpp --frozen` | flag | Alias for --locked |
| `mcpp --help` | flag | Show command usage. |
| `mcpp --home` | value | Select the mcpp home for this invocation. |
| `mcpp --locked` | flag | Fail if dependency resolution differs from mcpp.lock |
| `mcpp --managed-only` | flag | Fail if a toolchain, payload or plugin tool does not come from the ecosystem |
| `mcpp --no-color` | flag | Disable colored output |
| `mcpp --offline` | flag | Never touch the network (index refresh, downloads, toolchain install); = --deny network |
| `mcpp --protocol-version` | flag | Print the machine-output protocol this build speaks (JSON) |
| `mcpp --quiet` | flag | Suppress status output |
| `mcpp --system` | flag | Maintain the machine's system home (self init\|doctor\|store, toolchain install\|remove\|default, index update) |
| `mcpp --verbose` | flag | Show detailed progress on stderr |
| `mcpp --version` | flag | Print the mcpp version. |
| `mcpp version` | command | Show mcpp version |

## Source

- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
