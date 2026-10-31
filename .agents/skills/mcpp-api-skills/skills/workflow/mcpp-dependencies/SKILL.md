---
name: mcpp-dependencies
description: Manage mcpp source dependencies, exact selectors and version requirements, lockfiles, path/git sources, and runtime versus dev/build edges.
---

# Dependencies and Resolution

## Find the exact identity

```sh
mcpp search gtest
mcpp add compat.gtest@1.15.2 --dev
mcpp update
```

Confirm the version exists in the configured index before copying a package
example. `add` and `remove` use exact selectors `[namespace.]name`; search is
the substring operation. Keep a fully qualified dependency key as one key.

## Declarative sources

```toml
[dependencies]
math = { path = "../math" }
util = { git = "https://example.org/team/util.git", rev = "<commit>" }

[dev-dependencies]
"compat.gtest" = "1.15.2"
```

Replace the example git URL and commit with the actual repository. Published
version requirements and exact release keys have distinct grammars. Do not
invent wildcard forms such as `1.2.x`; check the accepted version parser.

## Three graphs

| Edge | Purpose |
|---|---|
| dependencies | Product code and usage requirements |
| dev-dependencies | Tests |
| build-dependencies | Programs/modules used on the build host |

Build tools declared with `tools` and host modules are host artifacts. `tools`,
`artifacts`, `host-module`, `reexport`, linkage and feature options each affect
a specific edge contract; do not treat them as ordinary link libraries.
Use `mcpp-rule-packages` for build-program libraries and reexports.

## Lock and constraints

`mcpp update [pkg]` deliberately re-resolves and writes the lock. `--locked`
(`--frozen`, `MCPP_LOCKED=1`) rejects differences; `--offline` denies network.
Do not promise that a lock alone makes every tool and payload available offline.

`default-features = false` applies to one edge; feature requests elsewhere
remain active. Public usage propagation is not the same as build-time reexport.

## References

[Interface reference](references/interfaces.md).

- [05-dependencies.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/05-dependencies.md)
- [specs/package-identity.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/package-identity.md)
