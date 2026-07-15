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

The first-run default is host-aware: Linux x86_64 → `gcc@16.1.0` (glibc — the
native ABI, so X11/GL/system libraries link out of the box); other Linux
arches (aarch64, …) → `gcc@15.1.0-musl` (self-contained, fully static);
macOS and Windows → `llvm@20.1.7`. Fully-static musl output stays one flag
away on any Linux host: `mcpp build --target x86_64-linux-musl`.

Subsequent builds do not trigger this process again.

> [!TIP]
> In CI or offline environments, you can disable automatic installation by setting `MCPP_NO_AUTO_INSTALL=1`. With this set, if no toolchain is installed, `mcpp build` fails immediately instead of making any network requests.

## The Identity Model: Toolchain × Target

Two orthogonal axes name everything:

- **toolchain** = `family@version`, family ∈ `gcc | llvm | msvc` — *who compiles*
- **target** = a triple `arch-os[-env]` (e.g. `x86_64-linux-musl`,
  `x86_64-windows-gnu`, `aarch64-macos`) — *what it produces for*

Variants live in the target's `env` segment (`gnu | musl | msvc`), never in
the toolchain name. "Cross" is not a name either — it's just the relation
`host ≠ target`, and the same command works for both. Legacy spellings
(`musl-gcc`, `gcc@15.1.0-musl`, `mingw`, `mingw-cross`, `clang`,
`x86_64-w64-mingw32`) are **permanently accepted aliases** that normalize to
this model with a one-line `note:` hint.

## Manual Installation

```bash
mcpp toolchain install gcc 16.1.0           # host target (GNU libc on Linux)
mcpp toolchain install llvm 20.1.7          # LLVM/Clang, the default on macOS/Windows
mcpp toolchain install gcc 16 --target x86_64-linux-musl    # musl target payload
mcpp toolchain install --target x86_64-windows-gnu          # family omitted → the
                                            # target's convention pin (gcc@16.1.0)
```

Explicit installation is mostly for CI cache warm-up and offline prep —
`mcpp build --target <triple>` auto-installs whatever the target needs.

Version numbers support partial matching:

```bash
mcpp toolchain install gcc 15               # installs the highest 15.x.y version (15.1.0)
mcpp toolchain install gcc@16               # the @ form works too
```

## Switching the Default Toolchain

The default is a *pair* — toolchain axis + target axis (target omitted = host):

```bash
mcpp toolchain default gcc@16.1.0
mcpp toolchain default gcc 15               # partial version → highest installed match
mcpp toolchain default gcc@16 --target x86_64-linux-musl   # "default to fully-static musl"
```

The pair persists as `[toolchain] default = "gcc@16.1.0"` +
`default_target = "x86_64-linux-musl"` in `~/.mcpp/config.toml`. (Older
configs with combined spellings like `default = "gcc@15.1.0-musl"` keep
working unchanged.)

## Inspecting Toolchain Status

```bash
mcpp toolchain list
```

The output has two blocks — one per axis:

```
Toolchains:
  *  gcc 16.1.0              (default)
     gcc 15.1.0
     llvm 22.1.8

Targets:
     TARGET                  NOTE                  TOOLCHAIN         STATUS
     x86_64-linux-gnu        host                  gcc 16.1.0        installed
  *  x86_64-linux-musl       static                gcc 16.1.0        installed
     x86_64-windows-gnu      PE, static, cross     gcc 16.1.0        installed
     aarch64-linux-musl      static, cross         gcc 16.1.0        available
     riscv64-linux-musl      static, cross         —                 planned

Available toolchains (run `mcpp toolchain install <family> <version>`):
     gcc 15.1.0 / 13.3.0 / 11.5.0 / 9.4.0
     llvm 20.1.7
```

`*` marks the default pair. The Targets block is the live view of the target
vocabulary: `installed` payloads, `available` targets this host can install,
and `planned` targets that are registered but not yet shipped.

## Windows PE via MinGW-w64 (`x86_64-windows-gnu`, no Visual Studio required)

"MinGW" in mcpp is a **target**, not a toolchain name: `x86_64-windows-gnu`
— GCC producing Windows PE with the GNU CRT. The same identity works from
both hosts; which self-contained payload serves it is resolved automatically
(Windows host → winlibs UCRT build; Linux host → the from-source MSVCRT
cross toolchain, wine-verified in CI):

```bash
mcpp build --target x86_64-windows-gnu       # from Windows OR Linux
mcpp toolchain default gcc@16 --target x86_64-windows-gnu
# legacy spellings still accepted: mingw@16.1.0, mingw-cross@16.1.0,
# --target x86_64-w64-mingw32
```

It uses the regular GCC module pipeline (`gcm.cache`, `import std` via
libstdc++'s `bits/std.cc`). The target's default linkage is **static** —
the produced `.exe` is fully self-contained (no `libstdc++-6.dll` to ship,
runs directly under wine); `[build] linkage = "dynamic"` opts out.

In a manifest:

```toml
[toolchain]
windows = "gcc@16"            # gcc family on Windows = MinGW-w64
# legacy value "mingw@16.1.0" keeps working
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

## Targets & Cross Builds

```bash
mcpp build --target x86_64-linux-musl        # fully static ELF
mcpp build --target aarch64-linux-musl       # cross-arch (aarch64 on x86_64)
mcpp build --target x86_64-windows-gnu       # Windows PE from Linux
```

`--target` is validated against the known-target vocabulary (see the README
platform table, which mirrors it): a typo is a **hard error with a
suggestion** (`did you mean 'x86_64-linux-musl'?`) — never a silent host
build. Custom triples outside the vocabulary are allowed when an explicit
`[target.<triple>]` section declares them in `mcpp.toml`.

Each known target carries a convention: its pinned toolchain (installed on
demand) and its default linkage (`*-linux-musl` and `x86_64-windows-gnu`
default to static). An explicit `[target.<triple>]` section overrides both:

```toml
[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

A project can set its *default* build target — this is where "this project
ships fully-static" belongs (static output is a product property, not a
compiler-family property):

```toml
[build]
target = "x86_64-linux-musl"                 # ≙ cargo's build.target
```

Combined with `mcpp pack --mode static` this produces a fully static release
package; for a complete example, see
[`examples/03-pack-static`](../examples/03-pack-static/).

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
