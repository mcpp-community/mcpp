---
name: mcpp-environment
description: Inspect mcpp home selection, configuration and environment, separate user and system maintenance, and avoid changing unrelated global installations.
---

# Homes and Environment

## Inspect first

```sh
mcpp self env
mcpp self env --format json
mcpp self doctor
```

Resolve the executable and effective home before changing configuration. A PATH
shim or another home can select a different installation from the expected one.

`--home PATH` selects a home for one invocation. `MCPP_HOME` and configured/system
homes participate in selection; use the reported environment rather than
assuming `~/.mcpp` is always the active store.

## Initialization and machine policy

`self init` initializes/selects a remembered home; `--force` has a distinct
reset/overwrite meaning. Inspect existing state first. `self config` shows or
changes supported configuration such as mirror policy. Do not apply one mirror
to unrelated projects as a workaround without a demonstrated need.

`--system` is for supported system-home maintenance commands, not ordinary
root builds. It is restricted to the allowed self/toolchain/index operations.
Build as the project user rather than leaving administrator-owned outputs.

`self store dedupe` changes stored file representation; inspect its options and
scope before treating it as a read-only report.

## Relevant environment families

- Home/config: MCPP_HOME and explicit `--home`.
- Invocation constraints: MCPP_OFFLINE, MCPP_LOCKED, MCPP_DENY_EFFECTS.
- Configuration: toolchain/sysroot/platform and target-specific selectors.
- Observation: MCPP_CLIENT labels lock/result ownership.
- Build-program contract: resolved target/host, tool roles, dependency and
  payload paths; these are supplied by mcpp, not user-authored substitute facts.

See the local extracted reference for exact environment identifiers. Some are
internal/testing knobs; existence in source is not an instruction to set them.
Use the API helper when one exists and handle empty values on Windows.

## References

[Interface reference](references/interfaces.md).

- [25-home-indices-and-store.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/25-home-indices-and-store.md)
- [23-the-project-environment.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/23-the-project-environment.md)
- [specs/home-initialization.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/home-initialization.md)
