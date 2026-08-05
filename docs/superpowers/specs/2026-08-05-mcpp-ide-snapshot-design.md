# Read-only IDE Snapshot Design

**Status:** Approved for M0 implementation

**Date:** 2026-08-05

## Problem

Editor integrations currently infer mcpp workspace state from human build output,
`compile_commands.json`, and filesystem conventions. Those heuristics cannot tell
whether a compilation database is current, which workspace member it belongs to,
or whether generating it would install dependencies, run `build.mcpp`, or write
project state. With C++ modules, a wrong or incomplete CDB makes clangd select the
wrong standard library, SDK, module files, or header.

mcpp already owns the eventual source of truth in `BuildPlan`, but the only path
that constructs it is `prepare_build()`. That function also resolves and installs
toolchains and dependencies, materializes generated files, compiles and runs
`build.mcpp`, and writes lock/build artifacts. It is not a metadata API.

## Decision

Introduce a versioned IDE-specific protocol, starting with one one-shot command:

```text
mcpp ide snapshot --format json [selectors]
```

M0 is a strictly read-only workspace inspection. It does not claim to produce the
final BuildPlan. It reports declared workspace/package/target data, the selected
configuration, the expected CDB location, artifact availability, and structured
diagnostics. A later milestone will add explicit `mcpp ide prepare --format
ndjson` to materialize authoritative CDB/PCM/generated artifacts.

The protocol is owned by mcpp core. The VS Code extension will consume it later;
the extension must not define a second project model or parse human output.

## Alternatives Considered

### Wrap `prepare_build()` with JSON

Rejected. It would make a command presented as inspection install packages,
download toolchains, execute project code, and write lock/build artifacts. Adding
booleans around individual writes would still leave a large, fragile matrix of
implicit side effects.

### Publish `BuildPlan` directly

Rejected. `BuildPlan` is an internal execution model whose fields and invariants
may change with the Ninja backend. Serializing it directly would turn internal
refactors into protocol breaks and would expose paths that are not valid until
materialization succeeds.

### Implement a daemon, JSON-RPC service, or LSP now

Rejected for V1. One-shot CLI calls are sufficient for workspace open/config
change events, are easy to cancel, and avoid lifecycle and protocol negotiation
complexity. A long-running transport can be added after measured demand.

## M0 Architecture

The implementation has four boundaries:

1. `mcpp.ide.model` defines the public in-memory DTO. It does not import
   `mcpp.build.plan`.
2. `mcpp.ide.inspect` performs filesystem and manifest reads only. It locates the
   workspace, parses members, applies existing member-selection rules, and probes
   whether each selected member has `compile_commands.json`.
3. `mcpp.ide.snapshot` converts the DTO to a single deterministic JSON document
   and computes an opaque content-derived snapshot ID.
4. `mcpp.cli.cmd_ide` translates CLI selectors into an inspection request, writes
   exactly one JSON document to stdout, and maps protocol state to an exit code.

`mcpp.cli` remains a thin router. `prepare_build()`, dependency resolution,
toolchain resolution, Ninja generation, and CDB writing are unchanged in M0.

## Request Selectors

M0 accepts and echoes these selectors:

```text
--package, -p <member>
--workspace
--profile <name>
--target <triple>
--features <comma-separated-list>
--cap <comma-separated-provider-pins>
--include-dev-dependencies
--format json
```

Selectors that require final build resolution are echoed but not interpreted
beyond member selection. This is explicit in the capability list and artifact
state. `--format` defaults to `json`; any other value returns a structured
`MCPP_IDE_UNSUPPORTED_FORMAT` diagnostic.

Member selection follows existing command semantics:

- `-p` selects exactly the matching member by relative path or basename.
- `--workspace` selects every declared member and the root package when present.
- A virtual workspace with no selector selects all declared members.
- A rooted workspace with no selector selects the root package.
- A non-workspace project selects its root package.

The snapshot always lists all successfully parsed workspace members, even when a
selector narrows `selectedMembers`.

## Public JSON Contract

The top-level object contains:

```json
{
  "schemaVersion": 1,
  "kind": "mcpp.ide.snapshot",
  "snapshotId": "fnv1a64:0123456789abcdef",
  "state": "partial",
  "mcpp": {
    "version": "2026.8.5.2",
    "protocol": {"min": 1, "max": 1},
    "capabilities": [
      "workspace-inspection",
      "manifest-diagnostics",
      "compile-commands-location"
    ]
  },
  "request": {
    "root": "/absolute/request/path",
    "selectors": {
      "package": null,
      "workspace": false,
      "profile": null,
      "target": null,
      "features": [],
      "capabilities": [],
      "includeDevDependencies": false
    },
    "mode": "read-only"
  },
  "workspace": {
    "root": "/absolute/workspace/root",
    "manifest": "/absolute/workspace/root/mcpp.toml",
    "members": [],
    "selectedMembers": []
  },
  "artifacts": {
    "state": "partial",
    "compileCommands": []
  },
  "diagnostics": []
}
```

Clients must ignore unknown fields. `schemaVersion` changes only for incompatible
field or semantic changes. Additive optional fields remain schema 1. Capability
strings state which portions of the larger IDE design this binary implements.

Each member contains `name`, `version`, `root`, `manifest`, and declared targets.
Each target contains `name`, `kind`, and optional `main`. Target kinds are stable
wire strings: `library`, `binary`, `shared-library`, and `test-binary`.

Each CDB artifact contains `member`, absolute `path`, and `state`. M0 states are:

- `missing`: no CDB exists.
- `stale`: a CDB exists, but M0 has no mcpp-published freshness metadata.

M0 never reports a CDB as `ready`.

## Snapshot State

The top-level and artifact state machine is deliberately conservative:

- `partial`: manifests were inspected, but every selected CDB is missing.
- `stale`: at least one selected CDB exists but none can be freshness-verified.
- `unavailable`: no usable workspace snapshot can be formed because the root
  manifest is missing/invalid or the requested member is invalid.
- `ready`: reserved for a later `ide prepare` publication and never emitted by M0.

Valid partial/stale snapshots exit 0. Unavailable snapshots exit 3. Human CLI
usage errors such as bare `mcpp ide` retain exit 2.

## Diagnostics

Diagnostics have stable fields:

```json
{
  "code": "MCPP_IDE_MANIFEST_INVALID",
  "severity": "error",
  "message": "expected value",
  "source": "mcpp",
  "path": "/project/mcpp.toml",
  "range": {
    "start": {"line": 4, "column": 9},
    "end": {"line": 4, "column": 9}
  }
}
```

`path` and `range` are optional. Manifest parser locations are one-based and stay
one-based on the wire in schema 1. M0 defines these codes:

- `MCPP_IDE_MANIFEST_NOT_FOUND`
- `MCPP_IDE_MANIFEST_INVALID`
- `MCPP_IDE_MEMBER_MANIFEST_MISSING`
- `MCPP_IDE_MEMBER_MANIFEST_INVALID`
- `MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND`
- `MCPP_IDE_ARTIFACTS_MISSING`
- `MCPP_IDE_ARTIFACTS_UNVERIFIED`
- `MCPP_IDE_UNSUPPORTED_FORMAT`

Member manifest failures are diagnostics attached to the workspace. They become
fatal only when the failed member is selected or when no selected member remains.

## Snapshot Identity

The snapshot ID is opaque to clients. M0 serializes a canonical JSON value with
`snapshotId` omitted, then computes FNV-1a 64 and prefixes it with `fnv1a64:`.
This is an identity/checksum, not a security primitive. Absolute paths and input
content represented by the DTO participate, while wall-clock time does not.
Identical inspection results therefore produce identical IDs and fixtures.

## Read-only Guarantee

`snapshot` may perform only these operations:

- Resolve absolute/canonical paths.
- Search ancestors for `mcpp.toml`.
- Read root/member manifests.
- Test existence and type of known manifest/CDB paths.

It must not:

- Initialize or write `MCPP_HOME`.
- Create `.mcpp`, `.xlings.json`, `target`, CDB, lock, or snapshot files.
- Refresh indices or access the network.
- Resolve/install toolchains or dependencies.
- Materialize generated files.
- Compile or execute `build.mcpp`.
- Run a compiler, scanner, Ninja, or user target.

The implementation therefore must not import or call `prepare_build()`, config
initialization, xlings, fetcher, toolchain detection, or build execution modules.

## Testing

Unit tests construct temporary project trees and call `inspect_workspace()` and
JSON serialization directly. They cover single packages, rooted/virtual
workspaces, member selectors, malformed manifests with locations, missing member
manifests, deterministic snapshot IDs, target kind mapping, and partial/stale
artifact states.

E2E tests invoke the fresh mcpp binary. They parse stdout with Python when
available and have portable grep fallbacks. They assert:

- stdout is one JSON document on success and unavailable failure.
- no human status or ANSI text appears on stdout.
- invalid format/member/manifest paths produce stable diagnostics and exits.
- a virtual workspace selects all members by default.
- an existing CDB is stale, not ready.
- before/after project and isolated `MCPP_HOME` trees are byte/path identical.

The full unit/integration suite and focused E2E must run with the fresh binary
produced by `mcpp build`.

## Deferred Milestones

`mcpp ide prepare` will add explicit, authorized mutation: toolchain/dependency
resolution, `build.mcpp`, required generated sources, CDB, and module PCM. It must
publish artifacts atomically and must not compile ordinary implementation objects
or link final targets.

Later snapshots may adapt a resolved plan into versioned toolchain, compile-unit,
module graph, build action, and BMI DTOs. They must remain adapters over internal
models, not serialized internal structs. Package search/install will use a
separate package protocol.
