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
mcpp test               # build and run C++ tests discovered under tests/**/*.cpp (including tests/unit)
```

`mcpp test` does not run the shell end-to-end suite under `tests/e2e/`; run
those separately against the freshly built binary.

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
├── cli/                  command implementations
├── manifest/             manifest model, TOML parsing, and xpkg descriptors
├── lockfile.cppm         mcpp.lock
├── version_req.cppm      SemVer constraints
├── fetcher.cppm          fetcher façade
├── fetcher/              package/index download and installation
├── config.cppm           ~/.mcpp/config.toml
├── bmi_cache.cppm        cross-project BMI cache
├── bmi_cache/            cache storage and invalidation
├── dyndep.cppm           ninja dyndep generation
├── ui.cppm               progress bars and output formatting
├── build/                build orchestration and ninja backend
├── fallback/             fallback resolution paths
├── modgraph/             P1689 module scanning and dependency graph
├── pm/                   dependency resolver and package-management commands
├── platform/             platform and process abstractions
├── scaffold/             `mcpp new` templates and project creation
├── toolchain/            toolchain detection, fingerprinting, and std module
├── pack/                 mcpp pack implementation
├── publish/              mcpp publish and xpkg generation
└── libs/                 third-party dependencies (toml parsing, etc.)

tests/
├── unit/                 C++ unit and integration tests, generally grouped by subsystem
└── e2e/                  end-to-end shell scripts (run_all.sh is the CI entry point)
```

## Test Organization

Tests are split into two layers:

- **Unit and integration tests** are C++ files discovered by `mcpp test` under
  `tests/**/*.cpp`. They are generally named for the subsystem or module they
  exercise (for example, `test_pm_lock_io.cpp` and `test_toolchain_triple.cpp`).
- **E2E tests** live in `tests/e2e/NN_<feature>.sh` and exercise a real `mcpp`
  binary; `run_all.sh` is the CI entry point.

Choose focused unit and/or E2E coverage according to the contract changed. E2E
scripts may require the same sandbox, mirror, and capability setup used by CI.

Run a single e2e script:

```bash
MCPP=<fresh-mcpp-binary> bash tests/e2e/02_new_build_run.sh
```

Replace `<fresh-mcpp-binary>` with the absolute path to the binary built in the
previous step; on Windows that path names `mcpp.exe`.

## Issue and PR Guidelines

### Issues

File issues at [github.com/mcpp-community/mcpp/issues](https://github.com/mcpp-community/mcpp/issues), ideally including the following:

- The full output of `mcpp self env`
- The full output of the failing command (`MCPP_LOG_LEVEL=debug` gives more detail)
- Your operating system, distribution, and glibc version (check with `ldd --version`)

### Pull Requests

mcpp is in early iteration and its interfaces may change. Before submitting a PR, please note:

1. For changes touching the CLI or the `mcpp.toml` schema, open an issue first to align on direction.
2. Keep each PR focused on a single change; write commit titles in English imperative form (`fix: ...` / `feat: ...`).
3. For behavior changes or test documentation, run `mcpp test` and the relevant
   E2E scripts against a fresh binary before submitting. For documentation-only
   changes, recheck the examples and links; use `gh pr checks <pr-number>` for
   the PR's actual required checks.

## Community Resources

- [Community forum](https://forum.d2learn.org/category/20)
- Chat group QQ: 1067245099
- [mcpp-index](https://github.com/mcpplibs/mcpp-index) — the default package index
- [mcpplibs](https://github.com/mcpplibs) — the companion collection of modular C++ libraries
