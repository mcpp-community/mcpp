# 00 — Getting Started

> Go from install → new → build → run → pack in 5 minutes.

## Installation

You only need a Linux x86_64 or macOS ARM64 environment — no need to install GCC, xlings, or any other dependencies beforehand.
On its first run, mcpp installs the default toolchain into an isolated sandbox (`~/.mcpp/`).
Linux defaults to musl-gcc, while macOS defaults to LLVM/Clang.

We recommend installing via [xlings](https://xlings.d2learn.org), which keeps mcpp isolated from your system environment:

```bash
xlings install mcpp -y
```

Alternatively, use the one-line installer script (xlings is bundled, and everything is installed under `~/.mcpp/`):

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

For full installation instructions (including xlings install commands, Windows support, and more), see the ["Installation" section of the README](../README.md#install).

Once installation is complete, start a new shell session or run `source ~/.bashrc`, then verify:

```bash
mcpp --version
# mcpp 0.0.1
```

> [!TIP]
> If you get `command not found`, it usually means `~/.mcpp/bin` has not yet
> been added to the current shell's PATH. Restart your terminal, or run
> `source ~/.bashrc` (use `~/.zshrc` for zsh, or `exec fish` for fish) to
> apply the change. You can also invoke mcpp directly via its absolute path
> `~/.mcpp/bin/mcpp`.

## Creating a Project

```bash
mcpp new hello && cd hello
```

This generates the following directory structure:

```
hello/
├── mcpp.toml         ← project manifest
└── src/
    └── main.cpp
```

By default, `src/main.cpp` is a C++23 modular hello world:

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
# Finished release [optimized] in 1.6s

mcpp run
# Hello from hello!
# Built with import std + std::println on modular C++23.
```

The first build downloads the default toolchain (musl-gcc 15.1 on Linux, LLVM/Clang 20.1 on macOS),
showing progress and speed along the way. Once downloaded, all mcpp projects share the same sandbox.

## Incremental Compilation and Testing

```bash
mcpp build              # incremental build
mcpp clean              # clean target/
mcpp test               # compile and run tests/**/*.cpp (gtest style)
```

## Adding Dependencies

Declare dependencies in `mcpp.toml`:

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` automatically resolves SemVer constraints against the
[mcpp-index](https://github.com/mcpp-community/mcpp-index), fetches the source,
and adds it to the build graph. For a complete example, see `02-with-deps` in
[01 — Examples](01-examples.md).

## Producing a Release Package

`mcpp pack` bundles your build artifacts and runtime dependencies into a self-contained tarball that can be distributed independently:

```bash
mcpp pack                          # default bundle-project, includes the project's third-party .so files
mcpp pack --mode static            # fully static (musl)
mcpp pack --mode bundle-all        # fully self-contained, including libc and ld-linux
```

For the differences between the three modes and their artifact layouts, see [02 — Packaging and Release](02-pack-and-release.md).

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
