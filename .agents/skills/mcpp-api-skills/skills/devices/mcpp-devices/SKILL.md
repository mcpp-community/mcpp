---
name: mcpp-devices
description: Reach non-native artifacts through mcpp runners, named device actions, launch descriptions, exclusive access and declared runtime files.
---

# Devices and Runners

## Discover the project's operations

```sh
mcpp run --list-runners
mcpp why runners
mcpp run
```

The default runner is how the artifact is executed. Named runners are other
ways to reach it; names such as flash, monitor and debug come from packages,
not a fixed engine vocabulary. Only use a name the resolved project supplies.

```sh
mcpp run --runner flash
mcpp run --features hardware
```

These commands can affect a device. The skill does not authorize hardware writes
or long-lived sessions beyond the user's requested task.

## Package and project declarations

Packages supply argv tokens with `runner(token)` or `runner(name,token)` and
declare the payload/tools that provide the executable. The project can replace
a default runner under a target row or a named argv under its `runners` table.
Use `{}` for explicit artifact placement, otherwise the artifact is appended.

Use `runner_longlived(name)` for a process without a natural end and
`run_exclusive()` when the target admits only one user. Do not derive lifecycle
from a familiar operation name.

Runner resolution and action tool lookup are different contexts. A declared
runner program can be resolved from prepared payloads; do not assume a random
PATH shim belongs to this home. Check actual resolved execution in a launch.

## Tests and observation

`run --print-launch` describes execution without performing it. The injected
`MCPP_RUNTIME_FILES` contract supplies declared runtime files to participating
runners. A test runner must preserve status and output correctly; a flash-only
command does not prove the test executed.

`--no-runner` directly executes the artifact on the host, useful only when
that host can actually run its format/architecture. Record emulator versus
physical-device validation explicitly.

## References

- [41-devices.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/41-devices.md)
- [specs/concurrency.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/concurrency.md)
