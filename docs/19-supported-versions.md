# 19 — Supported Versions and Compatibility

This document states which releases are supported, for how long, and what may
change between them. It exists because a project adopting mcpp is asked these
questions by its own review process, and an answer that lives only in
maintainers' heads cannot be cited.

## Versioning

Releases are named `YYYY.M.D.N` — the date of the release and the ordinal of
that day's release. The scheme carries no compatibility promise in its digits:
`2026.9.4.1` is not "a minor release" of `2026.9.3.2`. What may and may not
change is stated below rather than encoded in the number.

## What is supported

| | |
|---|---|
| **Supported** | the most recent release |
| **Security-fixed** | the most recent release, and the last release of the preceding calendar month |
| **Unsupported** | everything older |

A release is superseded rather than withdrawn. Published assets and index
entries for older versions remain in place, because a project may have pinned
one and removing it would break a build that was working.

## What may change between releases

The engine's own interfaces are not all equally stable, and the difference is
worth stating precisely.

| Surface | Stability |
|---|---|
| `mcpp.toml` keys | Additive. An existing key keeps its meaning; an unrecognised key is reported, never silently ignored |
| CLI commands and flags | Additive. A removed spelling is kept as an alias |
| Machine output (`--message-format json`) | Versioned by `schemaVersion`; see [11](11-machine-output.md) |
| `build.mcpp` directive protocol | Versioned; see `kProtocolVersion`. An engine refuses a program declaring a **higher** version rather than guessing |
| `mcpp.lock` format | Versioned by `schemaVersion`; older files are migrated on read |
| Target table rows | Additive. A row's tier may rise; a row is not removed while a published package targets it |
| Build fingerprints, cache layout, `target/` contents | **Not an interface.** These change without notice, and nothing should parse them |

⚠️ A `build.mcpp` calling a function its engine's bundled `mcpp` module does not
have fails at the **compile** of the build program, not through a protocol
error. The protocol number governs directives on the wire; the typed API is
governed by which engine is installed. Both are stated here because the failure
a package author sees depends on which one they crossed.

## Reproducing a build

`mcpp.lock` records what a build resolved. `--locked` asserts that a resolution
matches it and fails naming the package that moved:

```
error: --locked was given and this resolution differs from mcpp.lock:
         mcpplibs.cmdline 0.0.1 -> 0.0.2
```

A release build, an audit or a CI job should pass `--locked`. It disables the
build fast path, so the assertion always runs.

⚠️ The lock does not yet constrain resolution — it records and verifies it.
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
