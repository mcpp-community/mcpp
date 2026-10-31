---
name: mcpp-cache
description: Inspect and clean mcpp project outputs and shared dependency/std/tool caches with explicit scope, stale previews, verification and size or age budgets.
---

# Cache Inspection and Cleanup

## Inspect before deleting

```sh
mcpp clean --report
mcpp clean --dry-run
mcpp cache dir
mcpp cache list --format json
mcpp cache verify
```

`clean --report` shows project bytes. `--dry-run` previews stale cleanup and
implies stale selection; it does not preview a full global cache deletion.
`clean --stale` removes state no recorded current configuration needs.

## Project versus global

Plain `clean` removes project target output. `clean --bmi-cache` additionally
affects the global build cache. Do not use it as the first response to any
compiler diagnostic.

`cache clean` defaults to dependency entries. `--std`, `--tools`, `--all` and
`--legacy` select other explicit scopes. Shared cache cleanup affects other
projects; preserve the user's requested scope.

Use `cache info <pkg>@<version>` for identity inputs and `cache verify` for
manifest/file integrity. Age/size maintenance uses `prune --older-than` or
`gc --max-size` / `--older-than`; read help for accepted units.

## Build cache policy

`build --cache global|local|off` changes use of dependency caching. The old
`--no-cache` spelling also clears the build directory; it is not an innocuous
synonym for ignoring one result. Do not mix this option with std BMI identity
or assume it clears every global entry.

Compare fingerprint inputs when investigating stale-looking output. Different
standard, toolchain, runtime, target and configuration can legitimately coexist.
A cache hit is not evidence that a first-install/offline scenario was tested.
Legacy `cache list --json` lacks the modern envelope; select the intended format.

## References

[Interface reference](references/interfaces.md).

- [09-commands-by-scenario.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/09-commands-by-scenario.md)
- [25-home-indices-and-store.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/25-home-indices-and-store.md)
