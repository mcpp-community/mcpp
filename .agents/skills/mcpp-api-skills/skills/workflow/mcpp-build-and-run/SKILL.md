---
name: mcpp-build-and-run
description: Build and run mcpp products with profiles, package and target selection, cache modes, runner options and argument forwarding.
---

# Build and Run

## Build before executing

```sh
mcpp build
mcpp run
mcpp run server --release -- --port 8080
```

`server` selects a product. `--target` selects a triple or target set. `-p`
selects a workspace member. Do not rewrite one axis to solve an error on another.

## Common variations

| Task | Invocation |
|---|---|
| Select profile | `mcpp build --profile release` |
| Activate feature | `mcpp run --features logging` |
| Select member | `mcpp build -p app` |
| Select machine | `mcpp build --target x86_64-linux-gnu` |
| Bound compile concurrency | `mcpp build --jobs auto` |
| Inspect configuration identity | `mcpp build --print-fingerprint` |
| Prepare compiler database | `mcpp build --configure-only` |
| Stream machine result | `mcpp build --message-format json` |
| Describe execution | `mcpp run --print-launch` |

`--configure-only` does prepare work and writes project output; it is not the
same interface as `emit build-database`, nor a guarantee of zero side effects.

## Lifecycle hooks

`[hooks]` can configure build_start, build_failed, build_finished and
during_build. A self-closing hook accepts a command string or a table with cmd
and timeout_seconds; during_build is spanning and can declare loop instead of
its own timeout. The table also controls enabled, side_effect and the positive
default timeout_seconds. Hooks execute user commands; they are different from
incremental actions and should not generate undeclared compiler inputs.

## Cache and reproducibility

`--cache global|local|off` controls dependency caching. The deprecated
`--no-cache` also clears the build directory; do not describe it as harmless
performance tuning. Use `--locked` to reject lock resolution changes and
`--offline` to deny network access; these constraints have different meanings.

## Running a non-native artifact

Use a supplied runner or named `--runner`; inspect `--list-runners` first.
`--no-runner` explicitly asks the host to execute the artifact directly and
does not make an incompatible architecture executable. Target sets build their
legs; run selects a reachable one rather than executing all of them.

For distributable execution use `--pkg-format`; that selection is distinct
from machine-output formatting and cannot be combined freely with no-runner.

## Result handling

`run` passes through program/runner status. Build failure before launch is 101;
startup failures and signals need their own interpretation. Use
`mcpp-machine-interface` for protocol/record parsing and launch environment.

## References

[Interface reference](references/interfaces.md).

- [01-getting-started.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/01-getting-started.md)
- [52-driving-mcpp-from-a-program.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/52-driving-mcpp-from-a-program.md)
