# mcpp

> A modern C++ module-first build tool — written in pure C++23 modules, fully self-hosted

**English** | [简体中文](README.zh-CN.md)

[![Release](https://img.shields.io/github/v/release/mcpp-community/mcpp)](https://github.com/mcpp-community/mcpp/releases)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-ok-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

| [Documentation](docs/) · [Getting Started](docs/00-getting-started.md) · [mcpp.toml Guide](docs/05-mcpp-toml.md) · [Examples](docs/01-examples.md) · [Toolchains](docs/03-toolchains.md) |
|:---:|
| [Package index mcpp-index](https://github.com/mcpp-community/mcpp-index) · [Module libraries mcpplibs](https://github.com/mcpplibs) · [Community Forum](https://forum.d2learn.org/category/20) · [Issues](https://github.com/mcpp-community/mcpp/issues) · [Releases](https://github.com/mcpp-community/mcpp/releases) |
| [![ci-linux](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml) [![ci-macos](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml) [![ci-windows](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml) |

<p align="center">
  <img src="https://github.com/user-attachments/assets/6c85896e-9a37-4f62-acfb-d37a4eae2363" alt="mcpp demo" width="720">
</p>

## Highlights

- **Native C++23 module support** — `import std` handled automatically, file-level incremental builds, automatic module dependency analysis, zero manual configuration
- **Pure modular self-hosting** — mcpp itself consists of 43+ C++23 modules and builds itself; the module pipeline is battle-tested
- **Works out of the box** — one-command install, bundled GCC 16 / LLVM 20 toolchains downloaded into an isolated sandbox, never polluting your system
- **Integrated dependency management** — SemVer constraint resolution, lockfile, cross-project BMI cache, custom package indices
- **Multi-package workspaces** — unified lockfile and version management for larger projects

## Why mcpp

mcpp is built specifically for **C++23 module-first development**. If you want to use `import std`, module interface units (`.cppm`), module partitions, and other modern C++ features in your project, mcpp gives you a smooth, friendly experience on Linux and macOS ARM64:

- **Modular by default** — projects created by `mcpp new` use C++23 modules directly; `import std` just works
- **File-level incremental builds** — three-layer optimization based on P1689 dyndep (front-end dirty check + per-file scanning + BMI restat); only the modules that actually changed get recompiled
- **Create & build in one go** — `mcpp new hello && cd hello && mcpp build`; toolchains install automatically, no compiler or build-system setup required
- **A modular ecosystem** — [mcpplibs](https://github.com/mcpplibs) offers a growing set of directly `import`-able C++ module libraries, plus support for custom package indices

> [!NOTE]
> **Early-stage project** — mcpp is under active development; interfaces and behavior may change in future releases.
> Developers interested in modern C++ module-first build tooling are welcome to [contribute](#contributing).
> Questions / feedback / ideas — drop a note in [issues](https://github.com/mcpp-community/mcpp/issues).

## Getting Started

### Install

**Option 1: install via xlings (recommended)**

```bash
xlings install mcpp -y
```

<details>
<summary>Don't have xlings yet? Click for the install command</summary>

**Linux / macOS**
```bash
curl -fsSL https://d2learn.org/xlings-install.sh | bash
```

**Windows — PowerShell**
```powershell
irm https://d2learn.org/xlings-install.ps1.txt | iex
```

> More about xlings → [xlings.d2learn.org](https://xlings.d2learn.org)

</details>

**Option 2: one-line install script**

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

Installs into `~/.mcpp/` and adds it to your shell PATH. Deleting `~/.mcpp` uninstalls cleanly.

**Option 3: let an AI assistant install it for you**

Copy the following prompt to your AI coding assistant (Claude Code / Cursor / Copilot, etc.):

```
Read the README of https://github.com/mcpp-community/mcpp,
then install mcpp for me and create a C++23 module project, build and run it.
The repo's .agents/skills/mcpp-usage/SKILL.md has a detailed usage guide.
```

### Create, build & run a project

```bash
mcpp new hello
cd hello
mcpp build
mcpp run
```

> Note: the first build initializes the environment and fetches the toolchain, which may take a while.

### Project layout

```
hello/
├── mcpp.toml             ← project manifest
└── src/
    └── main.cpp          ← import std; works directly
```

```toml
# mcpp.toml
[package]
name = "hello"

[targets.hello]
kind = "bin"
main = "src/main.cpp"
```

### Using module libraries

Add a two-line dependency to `mcpp.toml` to pull in a community module library from [mcpplibs](https://github.com/mcpplibs):

```toml
[dependencies]
cmdline = "0.0.2"
```

Then `import` it directly in your code:

```cpp
import mcpplibs.cmdline;
```

> For more dependency options (version constraints, namespaces, Git references, local paths, etc.), see the [mcpp.toml guide — dependency management](docs/05-mcpp-toml.md).

## Feature Overview

<details>
<summary><b>Build system</b></summary>

- Native C++20/23 module support (interface units, implementation units, module partitions)
- Fully automatic precompilation and caching of `import std` / `import std.compat`
- Three-layer incremental optimization: front-end dirty check + per-file P1689 dyndep + BMI copy-if-different restat
- Fingerprinted BMI cache: hashed by compiler/flags/standard library, shared across projects
- Ninja backend: auto-generated build.ninja, parallel compilation
- compile_commands.json generated automatically (ready for clangd / ccls)
- First-class C support: `.c` files auto-detected, mixed C/C++ projects
- User-defined cflags / cxxflags / ldflags / c_standard

</details>

<details>
<summary><b>Toolchain management</b></summary>

- Bundled GCC 16.1.0 + LLVM/Clang 20.1.7, one-command install
- Fully static musl-gcc toolchain (default)
- Multiple versions side by side: `mcpp toolchain install gcc 16` / `mcpp toolchain install llvm 20`
- Isolated sandbox: all toolchains live in `~/.mcpp/registry/`, leaving the system untouched
- Per-platform selection: `linux = "gcc@16"`, `macos = "llvm@20"`
- GCC and Clang compile pipelines at parity (driven by the `BmiTraits` abstraction layer)

</details>

<details>
<summary><b>Package & dependency management</b></summary>

- SemVer constraint resolution: `^`, `~`, ranges, exact versions
- Three-stage resolution: constraint merging → multi-version mangling fallback → exact match
- Lockfile mcpp.lock (v2 format: index snapshot + namespaces)
- Namespace system: `[dependencies.myteam] foo = "1.0"`
- Custom package indices: `[indices] acme = "git@..."` / `{ path = "..." }`
- Project-level index isolation (`.mcpp/` directory, no global pollution)
- Dependency sources: index / Git / local path

</details>

<details>
<summary><b>Workspaces</b></summary>

- `[workspace] members = ["libs/*", "apps/*"]`
- Unified lockfile + unified target directory
- Centralized version management: `[workspace.dependencies]` + `.workspace = true`
- Selective builds: `mcpp build -p member-name`
- Config inheritance: toolchains, build flags, and indices cascade from root to members

</details>

<details>
<summary><b>Packaging & publishing</b></summary>

- `mcpp pack`: three Linux release modes — static (fully static musl) / bundle-project / bundle-all
- Fully static musl binaries: single-file distribution, no glibc dependency (Linux x86_64)
- `mcpp publish`: generates xpkg.lua + publishes to a package index
- Automatic RPATH fix-up via patchelf (Linux)

</details>

<details>
<summary><b>Developer experience</b></summary>

- `mcpp new` — create a modular project; `--template <pkg>[@ver][:<tmpl>]` uses a **library-provided template** (e.g. `--template imgui`); `--list-templates <pkg>` lists them
- `mcpp run [-- args]` — build and run
- `mcpp test [-- args]` — auto-discover and run tests
- `mcpp search` — search package indices
- `mcpp add / remove / update` — dependency management
- `mcpp explain E0001` — detailed error-code explanations
- `mcpp self doctor` — environment self-diagnosis

</details>

## Platform Support

| OS / arch        | GCC (glibc) | GCC (musl) | Clang / LLVM | MSVC |
|------------------|:-----------:|:----------:|:------------:|:----:|
| Linux x86_64     | ✅ | ✅ *default* | ✅ | — |
| Linux aarch64    | 🔄 | 🔄 | 🔄 | — |
| macOS arm64      | — | — | ✅ *default* | — |
| macOS x86_64     | — | — | 🔄 | — |
| Windows x86_64   | — | — | ✅ ¹ *default* | 🔄 |

✅ supported ｜ 🔄 planned

> *default*: the default toolchain on Linux is musl-gcc; release binaries are fully static musl builds.
> The default toolchain on macOS ARM64 / Windows x86_64 is LLVM/Clang.
>
> ¹ On Windows, Clang/LLVM currently requires an existing **MSVC BuildTools or Visual Studio** installation
> (providing the UCRT, Windows SDK, and MSVC STL). A zero-MSVC `llvm-mingw` route is planned
> ([discussion](https://github.com/mcpp-community/mcpp/issues)).

## Documentation

- [Getting Started](docs/00-getting-started.md) — install → new → build → run in 5 minutes
- [Examples](docs/01-examples.md)
- [Packaging & Release](docs/02-pack-and-release.md)
- [Toolchain Management](docs/03-toolchains.md)
- [Building from Source](docs/04-build-from-source.md)
- [mcpp.toml Guide](docs/05-mcpp-toml.md)
- [Workspaces](docs/06-workspace.md)

Full options for any command are available via `mcpp <cmd> --help`.

**AI-assisted learning**: send the following prompt to an AI coding assistant to get up to speed with mcpp quickly:

```
Read .agents/skills/mcpp-usage/SKILL.md and the docs/ directory of the
https://github.com/mcpp-community/mcpp repository,
then tell me how to create a C++23 module project with dependencies using mcpp.
```

## Contributing

Contributions via issues and PRs are welcome. The project accepts contributions developed with AI agents.

**Basic workflow**

1. Open an issue — for bug fixes, new features, or improvements, start a discussion in [issues](https://github.com/mcpp-community/mcpp/issues) first
2. Implement the change — fork the repo, create a branch, implement and verify (`mcpp build` + E2E tests)
3. Submit a PR — use `gh pr create` and make sure CI passes
4. CI must pass — PRs with failing CI will not be merged

**Commit message convention**: `feat:` / `fix:` / `test:` / `docs:` / `refactor:` prefixes

**AI agent contributions**: the repo's [`.agents/skills/mcpp-contributing/SKILL.md`](.agents/skills/mcpp-contributing/SKILL.md) provides a complete agent contribution workflow and project structure guide. Just send this prompt to your AI assistant:

```
Read .agents/skills/mcpp-contributing/SKILL.md of the
https://github.com/mcpp-community/mcpp repository,
then follow the guide to help me submit a contribution to mcpp.
```

## Community & Ecosystem

- [Community Forum](https://forum.d2learn.org/category/20) — chat group (QQ: 1067245099)
- [mcpp-index](https://github.com/mcpp-community/mcpp-index) — default package index
- [mcpplibs](https://github.com/mcpplibs) — collection of modular C++ libraries

### Acknowledgements

Dependencies and sources of inspiration:

- [xlings](https://github.com/d2learn/xlings) — toolchain / package-management foundation
- [mcpplibs.cmdline](https://github.com/mcpplibs/cmdline) — CLI framework
- [ninja](https://github.com/ninja-build/ninja) — underlying build engine
- [xmake](https://github.com/xmake-io/xmake) — cross-platform build tool
- [cargo](https://github.com/rust-lang/cargo) — Rust package manager
