---
name: mcpp-machine-interface
description: Drive mcpp from an agent or program with positive protocol detection, JSON/NDJSON, launch/result records, exit statuses, denied effects, locks and cancellation.
---

# Program and Agent Control

## Detect positively

```sh
mcpp --protocol-version
```

Parse stdout and require the expected `schemaVersion` and `kind`. An old binary
can print a human error on stdout; neither exit zero nor 'no error' proves a
protocol. Check envelope/kind versions and advertised features. At this baseline
features include target-sets, pkg-format-list and test-launch. Probe rather than
assuming the installed binary matches the suite's reviewed source.

## Use the correct channel

| Operation | Interface |
|---|---|
| self env, cache list, search, index list, toolchain list | `--format json` |
| Supported why topics | `--format json` |
| Build | `--message-format json`: event NDJSON, final `mcpp.build` |
| Tests | `--message-format json`: per-test NDJSON |
| Pack | `--message-format json`: `mcpp.pack` envelope |
| Run / one selected test launch | `--print-launch`: `mcpp.launch` |

Do not parse the entire build stream as one JSON document. Keep stderr separate;
it carries human narration. Legacy `--json` on cache/xpkg is a different payload,
not a universal alias. `--pkg-format` selects a container, not output encoding.

## Launch and persistent records

Launch `argv` as an argument array, use its `cwd`, and overlay the reported
environment changes on the caller's environment. `env` is not a full snapshot.
Never reconstruct an unquoted shell command from the array.

`target/.last-result.json` points to result records. Records distinguish running,
ok, failed and cancelled; multiple target legs can have an aggregate. A killed
process may leave a stale running record, so correlate process/liveness instead
of waiting forever. Launch description still builds first; it does not execute
the user program.

## Constraints and statuses

```sh
mcpp --deny network,exec-build-script build
mcpp --offline build
```

Static protocol effects state what may happen; result effects describe the run.
Query does not imply zero side effects: prepare can provision and execute build
programs. `read-project` is not denyable. Offline denies network; disabling auto
toolchain install is a narrower setting.

Denying `write-global-cache` is special: the build uses project-local caching
instead of necessarily stopping with status 5. Interpret the actual denied
operation, not a universal rule that every deny must fail.

For envelope fields, build/launch data and a minimal streaming consumer, read
[control records](references/control-records.md).

Interpret statuses per command: ordinary failure 1, usage 2, offline missing
dependency 4, denied effect 5, internal error 70; run build failure 101, program
status passthrough and startup failures need separate handling. POSIX signal
statuses are platform-specific. Consult the exit-code specification for 125-127
and special command paths rather than labeling every 127 'unknown command'.

## Concurrency and compatibility

Same-configuration builds serialize through a directory lock. `MCPP_CLIENT`
labels the caller. Run/test release build locks before executing the product.
Respect cancellation and read the resulting status; do not remove another
process's lock to make progress.

Only implemented why topics expose stable JSON: toolchain/sysroot, sources/tool/
payload, build-programs and compat. Do not fabricate shapes for runtime/deps/
runners. Additional/optional fields and unavailable features need explicit
handling rather than guessed fallbacks to human log parsing.

## References

[Interface reference](references/interfaces.md).

- [50-machine-output.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/50-machine-output.md)
- [52-driving-mcpp-from-a-program.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/52-driving-mcpp-from-a-program.md)
- [specs/exit-codes.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/exit-codes.md)
- [specs/concurrency.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/concurrency.md)
