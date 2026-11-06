---
subject: heterogeneous-build
status: active
---

# Launch configuration and named test runners

Status, 2026-11-06: implemented for candidate 2026.11.7.1. Focused local
controls pass; final public-dependency and cross-platform CI remain pending.

A consumer choosing a debugger runner needs the configuration of the program
selected to run. A separate `self env` query may describe a different profile
or the first leg of a target set instead of its reachable launch leg. The
launch producer already holds the selected build context and can describe it
without another planning protocol.

`mcpp.launch.data.configuration` therefore adds `triple`, `profile`,
`platform` and `runners` to both program and test launch descriptions.
The first three fields match the build result's configuration. `runners`
lists nonempty effective named-runner declarations even when no platform is
active. Older consumers can ignore the additive object; newer consumers test
for its presence. No device or debugger name acquires core semantics.

`mcpp test --runner <name>` uses the existing named-runner resolver. The same
choice applies to ordinary test execution, launch description and execution
of tests from an archive. A missing name is refused, rather than running the
test directly. `--runner` and `--no-runner` are contradictory and refused.
Timeouts, output capture, execution-host routing and test verdicts retain
their existing behavior. The platform or project owns the runner's command.

The IDE can use the additive configuration to choose a platform's `debuggee`
runner while preserving the selected target and profile, and reject a
different configuration in the second result. OpenKal platform 0.24 declares
that runner using existing platform fields. The core contains no OpenKal
debugger flag, loader path or JIT policy.

The fresh local candidate passes E2E 01, 333, 993 and 936. E2E 993 exercises a
real named runner, its returned command, ordinary and archived test execution,
argument preservation and refusals. E2E 936 passes with the inherited
`NO_COLOR=1` removed; its assertions remain unchanged. The build uses the OS
0.3 candidate source and does not establish final public installation.
An actual IDE-backend/core integration additionally passes eleven program
and test launch checks without a mock. The modified target-set E2E 963 and
the final packaged editor/runtime checks still require their full scopes.
