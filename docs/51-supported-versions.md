# 51 — Supported Versions and Compatibility

**Reader:** anyone with a policy question — a review process, an air-gapped
build, a pinned release.

**The question this chapter answers:** which releases are supported, what may
change between them, and what may not.

**Not here:** the exit-code table, which is [SPEC-003](specs/exit-codes.md), and
the machine-output schema, which is
[50 — Machine-Readable Output](50-machine-output.md).

This document states which releases are supported, for how long, and what may
change between them. It exists because a project adopting mcpp is asked these
questions by its own review process, and an answer that lives only in
maintainers' heads cannot be cited.

## Versioning

Releases are named `YYYY.M.D.N` — the date of the release and the ordinal of
that day's release. The scheme carries no compatibility promise in its digits:
`2026.9.4.1` is not "a minor release" of `2026.9.3.2`. What may and may not
change is stated below rather than encoded in the number.

## Support scope

| | |
|---|---|
| **Supported** | the most recent release |
| **Security-fixed** | the most recent release, and the last release of the preceding calendar month |
| **Unsupported** | everything older |

A release is superseded rather than withdrawn. Published assets and index
entries for older versions remain in place, because a project may have pinned
one and removing it would break a build that was working.

## Changes permitted between releases

The engine's own interfaces are not all equally stable, and the difference is
worth stating precisely.

| Surface | Stability |
|---|---|
| `mcpp.toml` keys | Additive. An existing key keeps its meaning; an unrecognised key is reported, never silently ignored. The exceptions are the deprecations below |
| CLI commands and flags | Additive. A removed spelling is kept as an alias |
| Machine output (`--message-format json`) | Versioned by `schemaVersion`; see [50](50-machine-output.md) |
| `build.mcpp` directive protocol | Versioned; see `kProtocolVersion`. An engine refuses a program declaring a **higher** version rather than guessing |
| `mcpp.lock` format | Versioned by `schemaVersion`; older files are migrated on read |
| Target table rows | Additive. A row's tier may rise; a row is not removed while a published package targets it |
| Build fingerprints, cache layout, `target/` contents | **Not an interface.** These change without notice, and nothing should parse them |

A `build.mcpp` calling a function its engine's bundled `mcpp` module does not
have fails at the **compile** of the build program, not through a protocol
error. The protocol number governs directives on the wire; the typed API is
governed by which engine is installed. Both are stated here because the failure
a package author sees depends on which one they crossed.

## Deprecations

A spelling or a reading of a manifest that is to be removed is deprecated
first, and the exceptions to "an existing key keeps its meaning" are exactly
the rows of this table (mcpp 2026.10.11.1+).

- A deprecated spelling keeps working. Each use in a manifest the build can
  edit (the root, a workspace member, a path package of the workspace) is
  warned about, naming the spelling that replaces it, the release that
  deprecated it and the release from which it is removed. `--strict` refuses
  it. `mcpp why compat` lists every use, in dependencies too.
- The removal is a CalVer month. At least two whole calendar months separate
  the release that deprecates a reading and that month, and the reading is
  removed in the first release of that month or later.
- The table is generated from one registry in the engine
  (`modules/manifest/src/compat/registry.cppm`); the engine's own tests fail
  once a release reaches a row's month while the reading is still present.

| Reading | Since | Removed from | Write instead |
|---|---|---|---|
| `workspace-position`: a workspace root's own `[toolchain]`, `[indices]`, `[profile.<name>]`, `[target.<triple>]` scalars or `[xlings]` reaching members by position | 2026.10.10.1 | 2027.1 | `[workspace.toolchain]`, `[workspace.indices]`, `[workspace.profile.<name>]`, `[workspace.target.<selector>]`, `[workspace.xlings]` |
| `host-module-in-dependencies`: a `[dependencies]` entry asking for `host-module = true` or for a `rule_module` feature, read as a build dependency | 2026.10.11.1 | 2027.1 | the same entry in `[build-dependencies]` |
| `abi-capability`: an `abi:` runtime capability (`abi:glibc`, `abi:libc=<c>`, `abi:cxxstdlib=<c++>`), read as a requirement on a target-side layer | 2026.10.26.1 | 2027.1 | `requires = ["mcpp:c-abi=<impl>"]` or `["mcpp:c++-abi=<impl>"]` |
| `undeclared-cxx-runtime`: a C++ runtime other than the build's own, named on its link line or needed by a library the line names, that no package declares | 2026.10.26.1 | 2027.1 | `requires = [{ layer = "mcpp:c++-abi=<impl>", visibility = "private" }]` in the package that needs it |
| `plugin-imports-from-build-dependencies`: a build plugin's library importing a package it lists in its own `[build-dependencies]` with `host-module = true` | 2026.10.11.1 | 2027.1 | the package in the plugin's `[dependencies]` |

## Reproducing a build

`mcpp.lock` records what a build resolved. `--locked` asserts that a resolution
matches it and fails naming the package that moved:

```
error: --locked was given and this resolution differs from mcpp.lock:
         mcpplibs.cmdline 0.0.1 -> 0.0.2
```

A release build, an audit or a CI job should pass `--locked`. It disables the
build fast path, so the assertion always runs.

The lock does not yet constrain resolution — it records and verifies it.
Pinning a resolution to the lock as an input is a separate change to the
resolver.

## Bill of materials

`mcpp emit sbom` writes a CycloneDX 1.5 document describing the **recorded**
resolution:

```bash
mcpp emit sbom -o sbom.json
```

It reads `mcpp.lock` rather than resolving again, because a document describing
a different graph from the one that was built is worse than no document. A
component whose licence mcpp does not know is emitted as `NOASSERTION` rather
than omitted: an absent field reads as "not examined".

## Offline and air-gapped use

`--offline` (or `MCPP_OFFLINE=1`) prevents every network access: index refresh,
package download and toolchain installation. A build that would need one of
those fails naming what it needed, rather than reaching out.

## Reporting a problem

Defects and security reports go to the issue tracker of the repository that
owns the component — the engine, the package index, or the package itself. A
report that names the version, the host, the target and the command is
actionable; one that does not usually results in a request for those four.

## Current limitations

- **`mcpp.lock` does not yet constrain resolution.** It records what a build
  resolved and `--locked` verifies that a new resolution matches it; pinning a
  resolution to the lock as an input is a separate change to the resolver.
- The SBOM describes the **recorded** resolution rather than a fresh one, so a
  document written after a manifest edit and before an `mcpp update` describes
  the older graph.
- A component whose licence mcpp does not know is emitted as `NOASSERTION`
  rather than omitted, because an absent field reads as "not examined".
