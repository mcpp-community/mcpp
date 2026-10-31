---
name: mcpp-manifest
description: Write and inspect mcpp.toml package identity, language standard, metadata, version floors, and configuration table ownership.
---

# The mcpp.toml Manifest

## Start with conventions

```toml
[package]
name = "hello"
version = "0.1.0"
standard = "c++23"
license = "Apache-2.0"
mcpp = ">=2026.10.31.3"
```

The floor above deliberately selects this suite's reviewed baseline; it is not
the historical introduction version of every feature. `mcpp` accepts a release
floor (`>=release`), not an exact pin or an arbitrary upper-bounded range.
Select a lower floor only after checking that every used capability exists.

`src/main.cpp` gives an inferred binary. Use explicit targets only for a needed
product shape. A package name is separate from its optional namespace; use full
dependency selectors when identities might collide.

## Standard and metadata

Use `[package] standard`, not the retained `[language] standard` spelling.
The root's standard applies to the module graph; dependency standards are not
independent compilation dialects. `c++latest` and `c++fly` depend on the actual
toolchain and are unsuitable as reproducible release requirements.

```toml
[package.metadata.mytool]
assets = "res"
```

Metadata is preserved for tools, not interpreted as an engine option. A reader
of a relative metadata path resolves it against that package's manifest root.

## Table ownership

| Table | Skill |
|---|---|
| `targets`, `lib`, `resources` | mcpp-targets |
| `modules` | mcpp-cxx-modules |
| `build`, `profile` | mcpp-build-options |
| dependency tables | mcpp-dependencies |
| `features`, `feature-deps` | mcpp-features |
| `workspace` and shared tables | mcpp-workspaces |
| `target`, `toolchain`, `platform`, `host` | toolchain/cross/platform skills |
| `test`, `runtime`, `pack`, `xlings` | corresponding task skill |

The local interface reference inventories parser readers and known-key tables.
It is not a claim that every accepted spelling is recommended or meaningful in
every context. Check scope and the consumer before moving a key between tables.

## Validate

Run the intended build/test with `--strict` when investigating schema warnings.
Unknown compatibility keys can warn; type errors and closed-set validation may
fail immediately. Do not silently remove unknown keys from published packages.

## References

[Interface reference](references/interfaces.md).

- [04-mcpp-toml.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/04-mcpp-toml.md)
- [specs/manifest-semantics.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/manifest-semantics.md)
