# 00 — Getting Started

> Go from install → new → build → run → pack in 5 minutes.

## Installation

Supported hosts are Linux x86_64 / aarch64, macOS ARM64, and Windows x86_64. You do not need to install GCC, xlings, or any other build dependency beforehand.
On its first run, mcpp installs a default toolchain into an isolated sandbox (`~/.mcpp/`). The choice is host-aware: Linux x86_64 uses `gcc@16.1.0`; other Linux architectures use `gcc@15.1.0-musl`; macOS uses `llvm@20.1.7`; Windows uses `llvm@20.1.7` when usable MSVC is available and otherwise uses `gcc@16.1.0` for `x86_64-windows-gnu`.

We recommend installing via [xlings](https://xlings.d2learn.org), which keeps mcpp isolated from your system environment:

```bash
xlings install mcpp -y
```

Alternatively, on Linux x86_64/aarch64 or macOS ARM64, use the one-line
installer script (xlings is bundled, and everything is installed under
`~/.mcpp/`):

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

The script does not support Windows; install through the PowerShell xlings
command in the README instead.

For full installation instructions (including xlings install commands, Windows support, and more), see the ["Installation" section of the README](../README.md#install).

Once installation is complete, start a new shell session, then verify:

```bash
mcpp --version
# mcpp <installed version>
```

> [!TIP]
> If the Unix release installer reports `command not found`, `~/.mcpp/bin` has
> not yet been added to the current shell's PATH. Restart your terminal, or run
> `source ~/.bashrc` (use `~/.zshrc` for zsh, or `exec fish` for fish) to apply
> the change; `~/.mcpp/bin/mcpp` is the direct path for that installer. If you
> installed through xlings, use the active xlings bin directory instead. On
> Windows, install through the PowerShell xlings command, restart PowerShell
> rather than using `source`, and verify the active command with
> `Get-Command mcpp.exe`.

## Creating a Project

```bash
mcpp new hello && cd hello
```

This generates the following directory structure:

```
hello/
├── mcpp.toml            ← project manifest
├── src/
│   └── main.cpp
└── tests/
    └── test_smoke.cpp   ← runs with `mcpp test`
```

The generated manifest contains only package metadata; mcpp infers a binary target from `src/main.cpp`. By default, that file is a C++23 modular hello world:

```cpp
import std;

int main() {
    std::println("Hello from hello!");
    std::println("Built with import std + std::println on modular C++23.");
}
```

## Building and Running

```bash
mcpp build
# Compiling hello v0.1.0 (.)

mcpp run
# Hello from hello!
# Built with import std + std::println on modular C++23.
```

The first build downloads the host-aware default toolchain, showing progress and speed along the way. Once downloaded, all mcpp projects share the same sandbox.

## Incremental Compilation and Testing

```bash
mcpp build              # incremental build
mcpp clean              # clean target/
mcpp test               # compile and run tests/**/*.cpp — one binary per file,
                        # framework-agnostic (bare main, or gtest via [dev-dependencies])
mcpp test <pattern>     # only tests whose name contains <pattern>
mcpp test --list        # enumerate tests without building
mcpp test --timeout 30  # kill a test still RUNNING after 30s (default 300; 0 = no limit)
mcpp test --build-timeout 120   # kill a compile/link still running after 120s (off by default)
```

The *run* half is bounded by default so an unattended CI job cannot be consumed by
a single hung test. The two deadlines cover different halves and neither implies
the other: `--timeout` bounds the test *process*, `--build-timeout` bounds one
ninja drive (the package build, the bulk test build, and each per-test build are
timed separately). **A link that never returns is a `--build-timeout` case; no
`--timeout` value stops it.**

`--build-timeout` is off by default, and the asymmetry is measured rather than
stylistic: a test binary running over five minutes is unusual, a cold dependency
build running over fifteen is ordinary (one mcpp-index member builds OpenCV from
source in 1019s on Linux and 1289s on Windows). A default ceiling would turn
slow-but-correct builds red. How long a build may take is a property of the
project, so the project says it. POSIX-only — the deadline runner has no
kill-by-handle path on Windows, where the value is ignored.

## Adding Dependencies

Declare dependencies in `mcpp.toml`:

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` automatically resolves SemVer constraints against the
[mcpp-index](https://github.com/mcpplibs/mcpp-index), fetches the source,
and adds it to the build graph. For a complete example, see `02-with-deps` in
[01 — Examples](01-examples.md).

## Producing a Release Package

`mcpp pack` bundles your build artifacts and runtime dependencies into a self-contained tarball that can be distributed independently:

```bash
mcpp pack                          # vendored by default: bundle project third-party .so files
mcpp pack --mode system            # rely on target-system libraries
mcpp pack --mode static            # fully static musl build
mcpp pack --mode self-contained    # bundle loader, libc, and dependencies
```

For the differences between the four modes and their artifact layouts, see [02 — Packaging and Release](02-pack-and-release.md). `bundle-project` and `bundle-all` remain accepted aliases for `vendored` and `self-contained`.

## Further Reading

- [01 — Examples](01-examples.md) — a collection of ready-to-run minimal projects
- [02 — Packaging and Release](02-pack-and-release.md) — building distributable artifacts
- [03 — Toolchain Management](03-toolchains.md) — switching compilers and managing multiple versions
- The full set of options for any command is available via `mcpp <cmd> --help`


## More Entry Points

- GUI quickstart: `mcpp new myapp --template imgui` (templates are distributed with the imgui library and their versions are aligned automatically;
  run `mcpp new --list-templates imgui` to see all templates the library provides, or use `--template imgui:docking` to select a specific one).
- Explaining default decisions: `mcpp why [toolchain|runtime|deps]`; host capability checkup: `mcpp self doctor`;
  machine-readable resolution manifest: the build artifact `target/<triple>/<fp>/resolution.json`.
- Offline operation: `mcpp --offline` or `MCPP_OFFLINE=1` prevents index refreshes, downloads, and toolchain installation.
