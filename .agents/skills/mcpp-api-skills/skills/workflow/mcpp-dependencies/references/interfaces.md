# Dependencies Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp add` | command | Add a dependency to mcpp.toml |
| `mcpp add --dev` | flag | Add to [dev-dependencies] (test-only, e.g. compat.gtest) |
| `mcpp add <pkg>` | required argument | Exact package spec, e.g. foo@1.0.0 or compat.gtest@1.15.2 |
| `mcpp remove` | command | Remove a dependency from mcpp.toml |
| `mcpp remove <pkg>` | required argument | Exact package selector [ns.]name |
| `mcpp resolve` | command | Re-resolve the build plan and explain it |
| `mcpp resolve --explain` | flag | Print resolved toolchain / runtime / deps / runners |
| `mcpp search` | command | Search packages in configured registries |
| `mcpp search --all-versions` | flag | List every published version instead of the latest few |
| `mcpp search --format` | value | json: the mcpp.search envelope (every version, licenses, repo) |
| `mcpp search <keyword>` | optional argument | Search keyword (substring match); optional with --format json, which then lists every package |
| `mcpp update` | command | Re-resolve dependencies and rewrite mcpp.lock |
| `mcpp update <pkg>` | optional argument | If given, update only that package |

## Manifest fields

| Interface | Details |
| --- | --- |
| `build-dependencies` | Applies to this package's inputs. Merged by key. |
| `dependencies` | Applies to this package's inputs. Merged by key. |
| `dev-dependencies` | Applies to this package's inputs. Merged by key. |

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
