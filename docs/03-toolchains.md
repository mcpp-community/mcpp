# 03 — Toolchain Management

> mcpp maintains an independent toolchain sandbox, fully isolated from the system PATH.

## Motivation

C++23 modules are fairly sensitive to compiler versions, and different releases of GCC / Clang differ noticeably in how they handle module semantics. The versions shipped by system package managers tend to lag behind, and keeping multiple versions side by side carries a maintenance burden. mcpp installs all toolchains into a single sandbox directory (`~/.mcpp/registry/data/xpkgs/`), letting each project pick the version it needs without touching the system environment.

## Automatic Installation

The first time you run `mcpp build`, if no toolchain is configured yet, mcpp automatically installs the default toolchain for your platform and sets it as the global default:

```
First run no toolchain configured — installing gcc@15.1.0-musl (musl, static) as default
Downloading xim:musl-gcc@15.1.0 [====>      ] 312 MB / 808 MB  3.7 MB/s
Default set to gcc@15.1.0-musl
```

Linux defaults to `gcc@15.1.0-musl`; macOS defaults to `llvm@20.1.7`.

Subsequent builds do not trigger this process again.

> [!TIP]
> In CI or offline environments, you can disable automatic installation by setting `MCPP_NO_AUTO_INSTALL=1`. With this set, if no toolchain is installed, `mcpp build` fails immediately instead of making any network requests.

## Manual Installation

```bash
mcpp toolchain install gcc 16.1.0           # GNU libc, for the default dynamic-linking case
mcpp toolchain install gcc 15.1.0-musl      # musl libc, for fully static builds
mcpp toolchain install musl-gcc 15.1.0      # equivalent to the line above
mcpp toolchain install llvm 20.1.7          # LLVM/Clang, the default toolchain on macOS
```

Version numbers support partial matching:

```bash
mcpp toolchain install gcc 15               # installs the highest 15.x.y version (15.1.0)
mcpp toolchain install gcc@16               # the @ form works too
```

## Switching the Default Toolchain

```bash
mcpp toolchain default gcc@16.1.0
mcpp toolchain default gcc 15               # with a partial version, picks the highest installed match
```

## Inspecting Toolchain Status

```bash
mcpp toolchain list
```

The output looks like this:

```
Installed:
     TOOLCHAIN               BINARY
     gcc 15.1.0-musl         @mcpp/registry/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++
  *  gcc 16.1.0              @mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++

Available (run `mcpp toolchain install <compiler> <version>`):
     TOOLCHAIN
     gcc 13.3.0
     gcc 11.5.0
     ...
```

The entry marked with `*` is the current default toolchain. `@mcpp/...` is shorthand for `~/.mcpp/...`, used to keep the output narrower.

## MinGW (Windows-native GCC, no Visual Studio required)

On Windows, `mingw` installs a self-contained MinGW-w64 GCC (winlibs
standalone build, UCRT runtime) into mcpp's sandbox — the same managed-xpkg
model as `gcc`/`llvm`, no Visual Studio needed:

```bash
mcpp toolchain install mingw 16.1.0
mcpp toolchain default mingw@16.1.0
```

It uses the regular GCC module pipeline (`gcm.cache`, `import std` via
libstdc++'s `bits/std.cc`). Produced binaries statically link libstdc++ and
libgcc by default, so they run standalone — no `libstdc++-6.dll` needs to
travel next to your exe (opt out with `[build] static_stdlib = false`).
`[build] linkage = "static"` upgrades that to a fully static link.

In a manifest:

```toml
[toolchain]
windows = "mingw@16.1.0"
```

## MSVC (System Toolchain, Windows)

MSVC is different from every other toolchain mcpp manages: it is a **system
toolchain**. mcpp locates and identifies an installed Visual Studio / Build
Tools — it never installs, updates, or removes MSVC itself.

```bash
mcpp toolchain default msvc
```

On a machine with MSVC installed, mcpp auto-locates it (via `vswhere.exe`,
then `VSINSTALLDIR`/`VS*COMNTOOLS`, then the standard install paths),
identifies the versions involved, and persists the stable spec `msvc@system`:

```
Detected   msvc 19.44.35211 (VS 2022 BuildTools) (VC tools 14.44.35207)
           cl: C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe
           import std: available (std.ixx)
Default    set to msvc@system (was: llvm@20.1.7)
```

If MSVC is **not** installed, mcpp prints installation guidance instead
(Visual Studio Installer with the *Desktop development with C++* workload, or
`winget install Microsoft.VisualStudio.2022.BuildTools`) and exits non-zero —
install it yourself, then re-run the command.

`mcpp toolchain list` shows the detected MSVC in a separate `System:` section,
and `mcpp self doctor` reports its status on Windows. In a manifest you can
pin it per-platform:

```toml
[toolchain]
windows = "msvc@system"
```

`msvc@<prefix>` (e.g. `msvc@19.44`) acts as a pin-verify: mcpp still uses the
newest installed VC tools, but errors if the detected version doesn't match
the prefix.

Since 0.0.90, **native cl.exe builds work**: mcpp synthesizes the
INCLUDE/LIB environment from the detected VC tools + Windows SDK (no
`vcvarsall` involved), stages `std.ixx`/`std.compat.ixx` as `.ifc` BMIs,
compiles `.cppm` module units via `/interface /TP /ifcOutput`, scans with
`/scanDependencies`, and links with `link.exe`/`lib.exe` through response
files. `[build] linkage = "static"` selects the `/MT` CRT. A missing Windows
SDK fails the build with installation guidance (`mcpp self doctor` reports
SDK status).

## Project-Level Version Pinning

If a project needs to pin a specific version rather than rely on the global default, declare it in the project's `mcpp.toml`:

```toml
[toolchain]
default = "gcc@16.1.0"

# you can also dispatch by platform
[toolchain]
linux = "gcc@15.1.0-musl"
macos = "llvm@20"
```

A project-level declaration takes precedence over the global default configuration.

## Cross-Toolchain Builds

```bash
mcpp build --target x86_64-linux-musl
```

mcpp reads the `[target.x86_64-linux-musl]` section in `mcpp.toml`, overriding the default toolchain and linkage settings. Combined with `mcpp pack --mode static`, this lets you produce a fully static release package; for a complete example, see [`examples/03-pack-static`](../examples/03-pack-static/).

## Uninstalling

```bash
mcpp toolchain remove gcc@16.1.0
```

## Resetting the Sandbox

```bash
rm -rf ~/.mcpp                              # remove the entire sandbox
mcpp build                                  # the next build triggers first-run installation again
```

## Environment Variables

mcpp's runtime behavior can be adjusted with the following environment variables:

| Variable | Purpose |
|---|---|
| `MCPP_HOME` | Override the sandbox location (default `~/.mcpp/`); an absolute path takes top priority |
| `MCPP_NO_AUTO_INSTALL=1` | Disable automatic toolchain installation; useful for CI and offline environments |
| `MCPP_NO_COLOR=1` / `NO_COLOR=1` | Disable colored output |
| `MCPP_LOG=trace\|debug\|info\|warn\|error` | Log level |

When `MCPP_HOME` is not set explicitly, mcpp locates the sandbox automatically based on the parent directory of the binary (after a release tarball is extracted to `~/.mcpp/`, `~/.mcpp/` is the home), so the release build runs without any environment variable configuration.


## ABI Capability Enforcement

A dependency can declare an `abi:<name>` capability (for example, `compat.glfw` declares `abi:glibc`). When the resolved toolchain's ABI does not satisfy any dependency's abi requirement, the build **fails early** with a suggested fix (for example, a musl-static toolchain encountering an abi:glibc dependency), replacing deeper link/header errors. Inspect with: `mcpp why toolchain`.
