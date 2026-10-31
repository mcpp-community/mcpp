---
name: mcpp-commands
description: Find public mcpp commands and option ownership, distinguish global flags, and forward arguments without confusing product names with target triples.
---

# Command Routing

Start with `mcpp --help`, then `mcpp <command> --help`. The exact command/option
inventory for the reviewed source is in [the interface reference](references/interfaces.md).

## Choose the command by intent

| Intent | Command | Focused skill |
|---|---|---|
| Create | `new` | projects-and-templates |
| Compile or execute | `build`, `run` | build-and-run |
| Discover/execute tests | `test` | tests |
| Add/remove/resolve | `add`, `remove`, `update`, `search`, `resolve` | dependencies |
| Distribute | `pack`, `publish`, `emit xpkg`, `emit sbom`, `xpkg parse` | packaging / publishing |
| Select compilers | `toolchain` | toolchains |
| Inspect/clean caches | `clean`, `cache` | cache |
| Manage indices | `index` | indices |
| Inspect environment | `self` | environment |
| Explain decisions | `why`, `explain` | troubleshooting |
| Export a plan | `emit build-database` | build-database |

Use full `mcpp-` prefixes when selecting the focused skill named in this table.

## Keep the axes distinct

```sh
mcpp run server --target x86_64-linux-gnu -- --port 8080
mcpp test smoke -- --test-argument
mcpp --offline build --locked
```

`server` names a product; `--target` selects the machine/platform axis. `-p`
selects a workspace package. Arguments after `--` belong to the program or test.

`--offline`, `--locked` (`--frozen`), `--home`, `--deny`, output verbosity and
protocol discovery have invocation-wide handling. Do not assume an option
accepted by one subcommand is accepted by every other subcommand.

## Boundaries

Backend helpers such as BMI commands, `dyndep`, `coff-def` and `platform-step`
are not ordinary user entry points. `stage` has a public action use through
`${mcpp.self}` and is documented with build actions instead.

For program consumers, do not guess JSON switches: queries use `--format json`,
build/test/pack use `--message-format json`, and launch prints its own envelope.

## References

[Interface reference](references/interfaces.md).

- [09-commands-by-scenario.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/09-commands-by-scenario.md)
- [50-machine-output.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/50-machine-output.md)
