---
name: mcpp-indices
description: Configure, inspect, update and pin mcpp package indices, distinguishing project registries from globally synchronized repositories.
---

# Package Indices

## Inspect before modifying

```sh
mcpp index list
mcpp index status
mcpp index list --format json
```

Use the enveloped list for program consumers. `status` inspects local freshness
offline; an update is a network operation.

## Project configuration

```toml
[indices]
team = "https://example.org/team/index.git"
local = { path = "../index" }

[dependencies.team]
internal = "1.0.0"
```

Use real URLs and available package versions. A project index and a package
namespace are related through identity rules, not interchangeable strings.

```sh
mcpp index add team https://example.org/team/index.git
mcpp index update team
mcpp index pin team
mcpp index unpin team
```

`pin` records a custom index revision in the manifest, defaulting to the current
locked revision when no revision is supplied. `remove` edits configuration; it
does not mean the same thing as uninstalling all packages from that index.

## Boundaries

The optional name on `index update` limits project custom indices only. Global
repositories still synchronize as a whole. Do not promise a single global
repository refresh if the provider cannot perform one.

Use `mcpp-environment` for home-level index configuration and `mcpp-dependencies`
for selector and lock semantics. Check offline failure instead of silently
dropping a registry or substituting a similarly named package.

## References

[Interface reference](references/interfaces.md).

- [25-home-indices-and-store.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/25-home-indices-and-store.md)
- [05-dependencies.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/05-dependencies.md)
