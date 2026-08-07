# 11 - IDE Protocol

This document describes the IDE protocol implemented by the current `mcpp`
binary. It is intended for IDE integrations, language-tool launchers and other
machine clients. The protocol is separate from the human-oriented CLI output;
clients must request and parse the documented format instead of scraping
progress text.

## 1. Scope and compatibility

The public commands are:

```text
mcpp ide snapshot [selectors]
mcpp ide configure [selectors]
```

Both commands currently use schema version `1`. For a snapshot response, clients
should check `schemaVersion`, `kind` and the `mcpp.protocol` range; for configure
events, clients should validate `schemaVersion`, `type`, `seq` and
`operationId`. Unknown fields must be ignored. `events`, `model`, `inspect` and
`publish` are internal C++ modules, not public CLI subcommands. The future `prepare`,
configuration index, cancellation and progress protocol are not implemented.

The project root is found by searching upward from the current directory for
`mcpp.toml`; the commands do not currently accept a project path argument.

## 2. Selectors

Both commands accept the following options:

| Option | Meaning |
| --- | --- |
| `-p, --package MEMBER` | Select one member by workspace-relative path or directory basename. |
| `--workspace` | `snapshot`: inspect every member. `configure`: currently rejected. |
| `--profile NAME` | Request a build profile. |
| `--target TRIPLE` | Request a target triple. |
| `--features LIST` | Comma-separated feature selectors. |
| `--cap LIST` | Comma-separated capability-provider pins. |
| `--include-dev-dependencies` | Include development dependencies explicitly. |
| `--format FORMAT` | `snapshot` accepts `json`; `configure` accepts `ndjson`. |

CSV selectors discard empty items but do not trim whitespace. There is no
`--offline` selector in this protocol.

When `configure` discovers `tests/**/*.cpp`, it automatically enables
development dependencies even without `--include-dev-dependencies`. This makes
test translation units use the same include paths and defines as a real test
build.

## 3. Read-only snapshot

`mcpp ide snapshot` defaults to `--format json` and does not resolve
dependencies, write project files or publish a CDB. A valid inspection exits `0` for
`partial`, `stale` or `configured`; an unavailable inspection exits `3`.

The top-level shape is:

```json
{
  "schemaVersion": 1,
  "kind": "mcpp.ide.snapshot",
  "snapshotId": "fnv1a64:...",
  "state": "partial",
  "mcpp": {
    "version": "2026.8.7.1",
    "protocol": {"min": 1, "max": 1},
    "capabilities": [
      "workspace-inspection",
      "manifest-diagnostics",
      "compile-commands-location"
    ]
  },
  "request": {"root": ".", "mode": "read-only", "selectors": {}},
  "workspace": {"root": ".", "manifest": "...", "members": [], "selectedMembers": []},
  "artifacts": {"state": "partial", "compileCommands": []},
  "diagnostics": []
}
```

`workspace.members[]` describes each parsed package with `name`, `version`,
`workspacePath`, `root`, `manifest` and `targets[]`. A target contains `name`,
`kind` (`library`, `shared-library`, `binary` or `test-binary`) and, when
available, `main`.

Each `artifacts.compileCommands[]` entry contains `member`, `path`, `state`
(`missing`, `configured` or `stale`) and optional `snapshotId` and
`configurationId` values.

The inspection state is aggregated as follows:

```text
missing artifacts -> partial
unverified regular root CDB -> stale
readable configured snapshot and reply CDB -> configured
```

`ready` is reserved for a future artifact-prepared state and is not currently
produced.

Diagnostics contain `code`, `severity`, `message`, `source: "mcpp"`, and may
include `path` and a zero-based `range` with `line` and `column` positions.
Known codes include `MCPP_IDE_MANIFEST_NOT_FOUND`,
`MCPP_IDE_MANIFEST_INVALID`, `MCPP_IDE_MEMBER_MANIFEST_MISSING`,
`MCPP_IDE_MEMBER_MANIFEST_INVALID`, `MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND`,
`MCPP_IDE_ARTIFACTS_MISSING`, `MCPP_IDE_ARTIFACTS_UNVERIFIED`,
`MCPP_IDE_ARTIFACTS_UNAVAILABLE`, `MCPP_IDE_SNAPSHOT_INVALID`,
`MCPP_IDE_SNAPSHOT_STALE` and `MCPP_IDE_UNSUPPORTED_FORMAT`.

The snapshot inspection checks metadata, project-root containment and CDB
readability. It does not recompute the manifest, lockfile, source set,
selectors or toolchain fingerprint. Therefore `configured` means that the last
published configured snapshot remains readable, not that the current inputs
have been revalidated. Clients should run `configure` after those inputs change.

## 4. Configure and NDJSON events

`mcpp ide configure` defaults to `--format ndjson`. Every stdout line is one
JSON object. A successful invocation emits, in order:

```text
operation-started
snapshot-published
operation-finished (success)
```

A failed invocation emits:

```text
operation-started
diagnostic
operation-finished (failed)
```

All events have a common envelope:

```json
{
  "schemaVersion": 1,
  "seq": 2,
  "type": "snapshot-published",
  "operationId": "operation-fnv1a64:..."
}
```

`seq` starts at `1` and increases strictly. All events for one invocation use
the same `operationId`.

The `operation-started` event identifies `operation: "configure"`. The
`snapshot-published` event includes `phase`, `state`, `projectId`,
`configurationId`, `snapshotId`, the content-addressed `compileCommands` path,
the compatibility CDB path, `compileCommandCount`, `toolchain` and
`toolchainFingerprint`. It may include:

```json
"stdModule": {
  "kind": "std-module",
  "path": "...",
  "state": "ready"
}
```

The successful `operation-finished` event includes `status: "success"`,
`operation`, `phase` and `configurationId`. A failure has a diagnostic event
with code `MCPP_IDE_CONFIGURE_FAILED`, followed by a finished event with
`status: "failed"` and `diagnosticCodes`.

Configure returns `0` after publication, `2` for a format error (before the
operation starts), and `3` for manifest, resolution, selector, tool, staging,
publication or unexpected failures. Cancellation, timeout and exit code `130`
are not protocol features yet.

## 5. IDs and published files

The IDs have different scopes:

- `projectId` identifies the physical workspace root.
- `configurationId` identifies normalized selectors, resolved profile/target,
  cache mode, language standard and toolchain fingerprint.
- `snapshotId` identifies the configured CDB publication and its provenance.
  The read-only snapshot's top-level `fnv1a64:*` is an inspection document ID;
  it is not the same ID type as the configured `snapshot-*` value.

After a successful configure, the relevant files are:

```text
<project>/.mcpp/ide/replies/compile_commands-<hash>.json
<project>/.mcpp/ide/replies/snapshot-<hash>.json
<project>/.mcpp/ide/current.json
<project>/compile_commands.json
<project>/.mcpp/ide/.lock
```

The reply CDB is the protocol source of truth. The root CDB is a compatibility
projection for existing clangd clients. `current.json` records
`schemaVersion`, `kind: "mcpp.ide.configured-snapshot"`, phase, IDs, project
root, both CDB paths, command count and toolchain identity.

Each individual JSON file is written through a temporary file and replacement.
The root CDB and `current.json` are separate replacement points, not one
cross-file operating-system transaction. Normal publication errors attempt to
restore the previous root CDB; a process crash between the two replacements can
still leave a new CDB alongside old metadata.

## 6. What configure does

Configure resolves the project and produces a CDB from the resolved `BuildPlan`
and compile flags. It does not compile ordinary project translation units or
link the final executable. It may nevertheless parse dependencies and the
toolchain, prepare a `build.mcpp` host tool, create or update caches, discover
tests, and stage standard-library or cached dependency BMIs.

The CDB includes ordinary sources and discovered test sources. Cached module
prerequisites are staged before the CDB is published. Uncached project or
dependency module BMIs are not fully built by this command, so module completion
may remain pending until a normal build or a future prepare command. Staging a
GCC `.gcm` or MSVC `.ifc` file does not promise that clangd can consume that
toolchain's module format.

`mcpp ide configure --workspace` is currently rejected. Select one member with
`--package` and run configure separately for each member.

## 7. Client lifecycle

The following behavior belongs in the IDE client and does not require another
mcpp protocol command:

1. Run `snapshot --format json` to discover members and existing artifacts.
2. For a workspace, use `workspace.members[].workspacePath` and run one
   `configure --package <workspacePath>` operation per member. Do not pass
   `--workspace` to configure. Serialize operations per member, or use bounded
   concurrency across independent members.
3. Reconfigure after the root/member `mcpp.toml`, `mcpp.lock`, `build.mcpp`,
   source-set/module declarations, selected profile/target/features/capabilities
   or toolchain selection changes. Debounce file events and reconfigure after an
   mcpp dependency or toolchain command completes. A plain source-content edit
   that does not change the source set or module graph does not require a new CDB.
4. Validate `seq`, correlate events by `operationId`, and ignore events from a
   superseded operation. The protocol has no cancellation yet, so the client
   should not start overlapping configure operations for the same member.
5. Treat `snapshot-published` as the publication boundary. The reply CDB is the
   protocol source of truth; a language server that requires a literal
   `compile_commands.json` may use `compatibilityCompileCommands`, but only after
   that event from the same operation. On startup or recovery, rerun configure
   instead of trusting a root CDB merely because it exists.
6. If configure fails, keep the previous usable clangd configuration, surface
   the structured diagnostic and mark it stale. A contention diagnostic may be
   retried with bounded backoff; an I/O/path diagnostic should be shown to the
   user instead of being retried as contention.
7. Do not interpret `configured` as full module readiness. Start clangd for the
   available TUs, show module support as pending when required BMIs are absent,
   and offer a normal `mcpp build` (or `mcpp test` for test-only preparation) when
   the user requests complete module semantics. Reconfigure after that command
   finishes. Do not start a full build silently during workspace discovery.
8. Keep stdout reserved for the requested JSON protocol. Human diagnostics may
   appear on stderr; clients should not parse them as events.

The protocol is intentionally one-shot today. Daemon mode, cancellation,
workspace fan-out, active freshness recomputation, a ready artifact snapshot and
last-known-good indexes remain future extensions.
