# 04 — Building from Source & Contributing

> mcpp is self-hosting — mcpp builds mcpp from source using mcpp itself.
> Any environment that already has a working mcpp binary can build from source.

## Prerequisites

Follow [00 — Getting Started](00-getting-started.md) to install a working copy of mcpp, then clone the repository:

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp
```

## Building and Testing

```bash
mcpp build              # compile the current source with the existing mcpp → ./target/.../bin/mcpp
mcpp run -- --version   # run the artifact you just built
mcpp test               # run tests/unit and tests/e2e
```

The first build automatically fetches the default toolchain; see [03 — Toolchain Management](03-toolchains.md) for details.

To produce a fully static binary identical to a release (the path taken by `release.yml`):

```bash
mcpp build --target x86_64-linux-musl
# → target/x86_64-linux-musl/.../bin/mcpp is a fully static ELF
```

## Source Layout

```
src/
├── main.cpp              entry point
├── cli.cppm              command dispatch and argument parsing
├── manifest.cppm         mcpp.toml parsing
├── lockfile.cppm         mcpp.lock
├── version_req.cppm      SemVer constraints
├── fetcher.cppm          dependency download (git / index / path)
├── config.cppm           ~/.mcpp/config.toml
├── bmi_cache.cppm        cross-project BMI cache
├── dyndep.cppm           ninja dyndep generation
├── ui.cppm               progress bars and output formatting
├── build/                build orchestration and ninja backend
├── modgraph/             P1689 module scanning and dependency graph
├── toolchain/            toolchain detection, fingerprinting, and std module
├── pack/                 mcpp pack implementation
├── publish/              mcpp publish and xpkg generation
└── libs/                 third-party dependencies (toml parsing, etc.)

tests/
├── unit/                 gtest unit tests for each .cppm module
└── e2e/                  end-to-end shell scripts (run_all.sh is the CI entry point)
```

## Test Organization

Tests are split into two layers:

- **Unit tests** live in `tests/unit/test_<module>.cpp`, corresponding one-to-one with `src/<module>.cppm` per module.
- **e2e tests** live in `tests/e2e/NN_<feature>.sh` and cover end-to-end behavior by exercising the real `mcpp` binary; `run_all.sh` is the CI entry point.

When changing any `.cppm` module under `src/`, check that the corresponding unit test covers your changes; for new features, prefer adding e2e cases.

Run a single e2e script:

```bash
cd tests/e2e
MCPP=$(realpath ../../target/x86_64-linux-musl/*/bin/mcpp) ./02_new_build_run.sh
```

## Issue and PR Guidelines

### Issues

File issues at [github.com/mcpp-community/mcpp/issues](https://github.com/mcpp-community/mcpp/issues), ideally including the following:

- The full output of `mcpp self env`
- The full output of the failing command (`MCPP_LOG=debug` gives more detail)
- Your operating system, distribution, and glibc version (check with `ldd --version`)

### Pull Requests

mcpp is in early iteration and its interfaces may change. Before submitting a PR, please note:

1. For changes touching the CLI or the `mcpp.toml` schema, open an issue first to align on direction.
2. Keep each PR focused on a single change; write commit titles in English imperative form (`fix: ...` / `feat: ...`).
3. Confirm that `mcpp test` passes in full before submitting.

## Community Resources

- [Community forum](https://forum.d2learn.org/category/20)
- Chat group QQ: 1067245099
- [mcpp-index](https://github.com/mcpp-community/mcpp-index) — the default package index
- [mcpplibs](https://github.com/mcpplibs) — the companion collection of modular C++ libraries
