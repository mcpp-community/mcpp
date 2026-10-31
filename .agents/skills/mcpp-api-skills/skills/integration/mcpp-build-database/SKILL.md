---
name: mcpp-build-database
description: Export mcpp S1 build databases and compile commands for editors or tools, watch declared inputs and distinguish read-only export from configure-only project writes.
---

# Build Database Integration

## Export a plan

```sh
mcpp emit build-database --format json
```

Validate the `mcpp.build-database` envelope. Its data contains the S1 database,
watch inputs and `inputs-fingerprint`. Consume the actual schema version; do
not infer a compile command by joining human compiler output.

The S1 description includes compilation and build-program information. Keep
host build programs distinct from target products when indexing sources.

## Refresh deliberately

Watch the declared files/environment inputs and compare the fingerprint when
deciding whether to re-query. A single root manifest mtime does not cover
dependency manifests, toolchain/configuration or build-program inputs.

`mcpp build --configure-only` writes project compilation metadata and prepares
a build directory. Database export does not write project output by default;
an explicit output request has its own contract. Use command help and the local
reference for S1 versus compile-commands selection and output destination.

## Side effects and failure

Export can prepare dependencies, use global caches, access the network or run
build programs. 'Does not write the project' is not 'performs no writes'. Apply
the caller's effect constraints and interpret denied effect separately from
an invalid project. Return diagnostics and keep a previous good database only
as visibly stale state, not silently current truth.

## Validate

Check a nonempty database for known source files, correct host/target entries,
argv boundaries and input fingerprint. Compare the project tree before/after
the default export. An exit-zero response with an empty graph is insufficient.
Use the vendored S1 schema and existing database tests when extending integration.

## References

[Interface reference](references/interfaces.md).

- [50-machine-output.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/50-machine-output.md)
- [specs/build-database.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/build-database.md)
