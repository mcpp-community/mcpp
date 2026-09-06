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
| 01 | [`examples/01-hello`](../examples/01-hello/) | Minimal single-file project with `import std` | The minimal package shape (`mcpp new` also emits `tests/test_smoke.cpp`) |
| 02 | [`examples/02-with-deps`](../examples/02-with-deps/) | Adds the `mcpplibs.cmdline` dependency to parse command-line arguments | `[dependencies]`, SemVer, `mcpp.lock` |
| 03 | [`examples/03-pack-static`](../examples/03-pack-static/) | Produces a fully static release package via `mcpp pack --mode static` | `[target.<triple>]` and `[pack]` configuration |
| 08 | [`examples/08-build-rules`](../examples/08-build-rules/) | Two rule packages and a project that uses both | `host-module = true`, `[build-dependencies]`, `mcpp::action` with `role = "check"` |
| 09 | [`examples/09-heterogeneous`](../examples/09-heterogeneous/) | One computation on a device, in four programming models, with a CPU fallback in each | `accel`, constrained source globs, the seam module, rule packages from `mcpp:plugins`, `cfg(accelerator = …)` |
| 09a | [`…/cuda`](../examples/09-heterogeneous/cuda/) | A CUDA kernel behind a seam module | `mcpp.rules.cuda`, `mcpp::action` with `role = "object"`, the driver stated as a fact and a floor |
| 09b | [`…/vulkan`](../examples/09-heterogeneous/vulkan/) | The same computation as a Vulkan compute shader, on a GPU or on the CPU | `mcpp.rules.spirv`, `mcpp::action` with `role = "source"`, generated headers, a software driver as a payload |
| 09c | [`…/sycl`](../examples/09-heterogeneous/sycl/) | The same computation as a SYCL kernel, compiled by a second compiler | `mcpp.rules.sycl`, the `.sycl` device extension, a chained `mcpp::action` for the device link, `compat:sycl-runtime` |
| 09d | [`…/hip`](../examples/09-heterogeneous/hip/) | The same computation in HIP, reaching an NVIDIA device | `mcpp.rules.hip`, HIP as a header layer over the CUDA runtime, a two-chunk `accel` |

## Suggested Reading Order

We recommend reading them in numerical order:

1. **`01-hello`** shows the minimal package skeleton (`mcpp.toml` and
   `src/main.cpp`) and demonstrates the basic usage of `import std`. The current
   `mcpp new` scaffold also emits `tests/test_smoke.cpp`.
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
