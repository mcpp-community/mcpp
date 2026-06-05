# 01 — Examples

> The repository's [`examples/`](../examples) directory provides a set of
> progressively more advanced minimal projects, covering common scenarios from
> a single-file `import std` to a fully static release package. Each example can
> be entered on its own and built with `mcpp build`.

## How to Run

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp/examples/01-hello
mcpp build && mcpp run
```

Each example ships with its own README that only explains the new concepts it
introduces relative to the previous one. Common material such as installation
steps and toolchain initialization lives in
[00 — Getting Started](00-getting-started.md) and is not repeated within the
examples.

## Example List

| # | Path | Description | Key Concepts |
|---|---|---|---|
| 01 | [`examples/01-hello`](../examples/01-hello/) | Minimal single-file project with `import std` | The default output structure of `mcpp new` |
| 02 | [`examples/02-with-deps`](../examples/02-with-deps/) | Adds the `mcpplibs.cmdline` dependency to parse command-line arguments | `[dependencies]`, SemVer, `mcpp.lock` |
| 03 | [`examples/03-pack-static`](../examples/03-pack-static/) | Produces a fully static release package via `mcpp pack --mode static` | `[target.<triple>]` and `[pack]` configuration |

## Suggested Reading Order

We recommend reading them in numerical order:

1. **`01-hello`** shows the minimal skeleton of an mcpp project (`mcpp.toml` and
   `src/main.cpp`) and demonstrates the basic usage of `import std`.
2. **`02-with-deps`** builds on the previous example by introducing an external
   dependency, covering the lock-file mechanism and how the modular package
   index works.
3. **`03-pack-static`** demonstrates how to package build artifacts into a
   standalone, independently distributable single-file binary; for packaging
   details, see [02 — Packaging and Release](02-pack-and-release.md).

## Adding a New Example

Example projects follow a consistent directory structure: `mcpp.toml` + `src/` +
`README.md`. To add a new example, create a numbered directory under
`examples/` (e.g. `04-xxx/`), briefly describe the concept it demonstrates in
its README, and then open a PR. For contribution guidelines, see
[04 — Build from Source & Contributing](04-build-from-source.md).
