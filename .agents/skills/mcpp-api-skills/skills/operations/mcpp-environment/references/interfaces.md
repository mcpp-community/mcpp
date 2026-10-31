# Environment Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp self` | command | Inspect and manage mcpp itself |
| `mcpp self config` | command | Show or modify mcpp's configuration: the xlings mirror and the retention of build outputs |
| `mcpp self config --cache-unused-days` | value | Collect build-cache entries no build used for DAYS days (default 30; 0 keeps all) |
| `mcpp self config --keep-days` | value | Sweep a project's build directories no configuration used for DAYS days (default 3; 0 keeps all) |
| `mcpp self config --mirror` | value | Set xlings mirror for mcpp's private registry |
| `mcpp self doctor` | command | Diagnose mcpp environment health |
| `mcpp self env` | command | Print mcpp paths and configuration; with --target, one configuration: toolchain, platform, tools, runner |
| `mcpp self env --format` | value | Machine-readable output (enveloped; see docs/50-machine-output.md) |
| `mcpp self env --host` | flag | Report the host graph's configuration: what build programs and tools are built for |
| `mcpp self env --host-platform` | value | The host graph's platform for this invocation (= MCPP_HOST_PLATFORM) |
| `mcpp self env --platform` | value | The target's platform for this invocation (= MCPP_PLATFORM): <name>@<version>, path:<dir>, none |
| `mcpp self env --target` | value; repeatable | Report this target's configuration (prepared, not built; the project's when run in one); a target set or several reports each leg |
| `mcpp self explain` | command | Show extended description for an error code |
| `mcpp self explain <code>` | required argument | Error code such as E0001, or a refusal's reason token such as platform-version-group |
| `mcpp self init` | command | Initialize or repair mcpp sandbox |
| `mcpp self init --force` | flag | Rebuild registry/caches; with --home, replace the default without deleting data |
| `mcpp self init --home` | value | Initialize PATH and remember it from the original home (does not migrate data) |
| `mcpp self store` | command | Compare this home's payloads with its upstreams |
| `mcpp self store dedupe` | command | List payloads an upstream has the same of; --apply links them |
| `mcpp self store dedupe --apply` | flag | Replace each by a link to the upstream's |
| `mcpp self version` | command | Show mcpp version |

## Environment variables

| Interface | Details |
| --- | --- |
| `MCPP_DENY_EFFECTS` | See the environment and command instructions. |
| `MCPP_HOME` | See the environment and command instructions. |
| `MCPP_HOST_PLATFORM` | See the environment and command instructions. |
| `MCPP_LOCKED` | See the environment and command instructions. |
| `MCPP_OFFLINE` | See the environment and command instructions. |
| `MCPP_PLATFORM` | See the environment and command instructions. |
| `MCPP_SYSROOT` | See the environment and command instructions. |
| `MCPP_TOOLCHAIN` | See the environment and command instructions. |
| `MCPP_VERBOSE` | See the environment and command instructions. |

## Source

- [src/build/build_program.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/build/build_program.cppm)
- [src/build/hostprogram.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/build/hostprogram.cppm)
- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
- [src/config.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/config.cppm)
