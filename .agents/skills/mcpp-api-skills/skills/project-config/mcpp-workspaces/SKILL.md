---
name: mcpp-workspaces
description: Configure mcpp workspace members, virtual roots, shared package/build/dependency layers, and member selection without leaking root-only settings.
---

# Workspaces

## Define members and shared metadata

```toml
[workspace]
members = ["libs/*", "apps/*"]
exclude = ["apps/experimental"]

[workspace.package]
version = "0.1.0"
license = "Apache-2.0"

[workspace.build]
defines = ["WORKSPACE_BUILD=1"]
```

```sh
mcpp build -p app
mcpp test --workspace
```

Member selectors resolve full identity, package name and directory according
to the command's rules. Disambiguate duplicate names with namespace.name or
directory. `run` requires a single member; build/test can select several.

## Root is not a shared layer

A root with `[package]` is itself a package. Its ordinary `[build]` does not
automatically become every member's configuration. Put supported common inputs
under `[workspace.build]`; declare common dependency specs under
`[workspace.dependencies]` and opt into inheritance on the member edge.

Shared scalars, lists, defines and tables do not merge identically: lists append
workspace entries first, member defines replace the same macro name, and member
scalar declarations win. The registry determines which keys can be shared.
Workspace toolchain/indices/profile/target/xlings layers need their respective
shared forms, not arbitrary nesting under build.

A shared local dependency has two declarations. At the workspace root:

```toml
[workspace.dependencies]
math = { path = "libs/math" }
```

In the consuming member's manifest:

```toml
[dependencies]
math = { workspace = true }
```

Paths in the shared declaration are anchored at the workspace root. The member
opts into the shared spec; simply writing a root dependency does not add it to
every member's graph.

## Selection and validation

Use `--workspace --exclude member` to omit members; do not combine exclusion
with `-p` selection. A virtual root has no product of its own.

Check an individual member in the graph it actually declares, then a workspace
selection. A no-test member can return success: assert that the expected tests
were discovered. An empty selection is not behavioral coverage.

## References

[Interface reference](references/interfaces.md).

- [07-workspace.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/07-workspace.md)
- [specs/manifest-semantics.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/manifest-semantics.md)
