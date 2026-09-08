# 08 — Testing

**Reader:** anyone with code that has to keep working.

**The question this chapter answers:** how tests are written and run, what mcpp
considers a test, and how to test something that does not run on this machine.

**Not here:** how a runner reaches a device — that is
[41 — Reaching a Device](41-devices.md) — and the schema of the machine-readable
stream, which is [50 — Machine-Readable Output](50-machine-output.md). This
chapter states which flag produces it and stops there.

Before: [05 — Dependencies and Resolution](05-dependencies.md) covers
`[dev-dependencies]`, which is how a test reaches a package the artifact does
not. After: [09 — Commands by Scenario](09-commands-by-scenario.md) is the
lookup for everything else.

## What mcpp considers a test

Every `tests/**/*.cpp` is a test: mcpp compiles each one into its own program
and runs it. A test passes when its program exits zero.

```
myproject/
  mcpp.toml
  src/…
  tests/
    test_parse.cpp        one program
    unit/test_span.cpp    another
```

There is no framework and no registration. A test may use one — `[dev-dependencies]`
is how it reaches it — but the contract mcpp holds is the exit code, which is
also why a test written for another framework needs no adapter.

`mcpp new` scaffolds `tests/test_smoke.cpp` so a project starts with the
directory in place.

## Running them

```bash
mcpp test                 # build and run every test
mcpp test parse           # only those whose name matches
mcpp test --list          # what would run, without building or running it
mcpp test -- --verbose    # everything after `--` goes to each test binary
```

Tests build with the same axes as `mcpp build`, so a test runs against the
configuration it is meant to check rather than against the default one:

| flag | what it selects |
|---|---|
| `--profile <name>` | `dev` (default), `release`, `dist`, or a `[profile.*]` the manifest declares |
| `--features <list>` | the feature set for the test build |
| `--target <triple>` | a target other than the host |
| `--accel <spec>` / `--no-accel` | the device backends the build targets |
| `--cap <list>` | pin a capability provider |

`--timeout <secs>` kills a test still running (default 300; `0` disables it) and
`--build-timeout <secs>` bounds the compile. A test that hangs is reported as a
failure with its own name, not as a job that stopped.

## Tests that reach packages the artifact does not

```toml
[dev-dependencies]
counters = { path = "../counters" }
```

A `[dev-dependencies]` entry is resolved for the test build and for nothing
else: it is not in the artifact, and a consumer of the package never sees it.
That is the difference between a test's dependency and the package's own, and it
is why a test framework does not become part of what a library ships.

[`examples/11-features`](../examples/11-features/) declares one and uses it.

## Testing on a target this machine cannot run

A test for a cross target or a bare-metal board is compiled for that target and
executed through a **runner** — an argv a board-support package supplies, which
mcpp performs with the test binary appended.

```bash
mcpp test --target thumbv7em-none-eabihf     # built for the board, run through its runner
mcpp test --no-runner                        # ignore the runner and execute directly
```

Nothing about the test changes. The same `tests/**/*.cpp` compiles for the
device, and the verdict is still the exit code — which is why a semihosting exit
or a QEMU exit code is what a bare-metal runner is chosen to produce.

`--no-runner` exists for a host that can execute the binaries natively and
should not pay for an emulator.

The runner itself, named runners, and what a board declares are
[41 — Reaching a Device](41-devices.md).

## Tests that cannot run beside each other

`mcpp test` runs test programs on a worker pool. One board on one probe, one
GPU, one serial port or a single-seat licence admits one user at a time, and two
workers reaching for it interleave rather than fail.

The package that owns the resource states this about itself, and `mcpp test`
then serialises those tests. A project never has to remember `-j1`.

## Reporting to a program

```bash
mcpp test --message-format json
```

One NDJSON record per test, for a CI job or an editor. The schema and its
version are [50 — Machine-Readable Output](50-machine-output.md); what belongs
here is only that the flag exists and that the human format is the default.

## Current limitations

- A test is one `.cpp` producing one program. mcpp does not discover cases
  inside a file, so a framework's per-case selection happens inside the program,
  through arguments after `--`.
- `--build-timeout` is POSIX-only.
- `--workspace-timeout` bounds a `--workspace` fan-out and reports what did run;
  it does not attribute the timeout to a member.
