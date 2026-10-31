---
name: mcpp-troubleshooting
description: Diagnose mcpp failures through resolved decisions, structured diagnostics, environment checks and targeted reproduction instead of speculative cache deletion.
---

# Troubleshooting

## Establish what failed

Record mcpp version, resolved executable/home, host, requested target, command,
exit status and the first causal diagnostic. Separate package resolution,
toolchain preparation, C++ compilation, linking, deployment and program failure.

```sh
mcpp self doctor
mcpp why toolchain
mcpp why sources
mcpp why build-programs
mcpp why compat
mcpp explain E_NOT_FOUND
```

Use the actual code from the diagnostic when requesting `explain`; not every
failure has the example code. `why` topics include runtime, deps and runners,
but not every topic has JSON output. The supported machine topics are listed
in `mcpp-machine-interface`.

## Narrow the hypothesis

| Observation | Next evidence |
|---|---|
| Package missing | Exact selector/version, configured indices, offline state |
| Wrong compiler/ABI | why toolchain, fingerprint, runtime/provider decisions |
| Compiler found but Ninja cannot start | Active home, installed Ninja executable and process PATH; see mcpp-toolchains |
| Generated source missing | Action inputs/outputs, generator status, role |
| Old configuration reused | Declared rerun files/env and graph fingerprint |
| Program cannot start | Launch argv/cwd/env, runner reachability, loader files |
| Feature appears absent | Effective feature requests and selected sources |
| Encoding/path failure | Actual platform/path code-page constraints |

Do not immediately clean caches, reinstall global tools or migrate dependencies.
Those actions can destroy the evidence and exceed the user's task. Change one
relevant axis and rerun the smallest meaningful reproduction.

## Report evidence accurately

Distinguish static implementation findings, successful compile, actual execution
and unavailable capability. Capture the source commit and toolchain when filing
an implementation bug. An empty workspace, skipped test or cached tool install
must not be presented as successful coverage of the failed path.

When modifying mcpp itself, read that checkout's contribution requirements;
this consumer-facing troubleshooting workflow does not replace them.

## References

[Interface reference](references/interfaces.md).

- [09-commands-by-scenario.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/09-commands-by-scenario.md)
- [51-supported-versions.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/51-supported-versions.md)
- [52-driving-mcpp-from-a-program.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/52-driving-mcpp-from-a-program.md)
