# 20 — Toolchain Management

**Reader:** anyone for whom which compiler runs matters — a version floor, a
second toolchain, a machine that must not use its own.

**The question this chapter answers:** which compiler will build this project,
how is it installed, and how do I choose another.

**Not here:** how the resolution is implemented, which is
[91 — Toolchain Internals](91-toolchain-internals.md), and how a target is
named, which is [21 — The Target Triple](21-the-target-triple.md).

> mcpp maintains an independent toolchain sandbox, fully isolated from the system PATH.

## Motivation

C++23 modules are fairly sensitive to compiler versions, and different releases of GCC / Clang differ noticeably in how they handle module semantics. The versions shipped by system package managers tend to lag behind, and keeping multiple versions side by side carries a maintenance burden. mcpp installs all toolchains into a single sandbox directory (`~/.mcpp/registry/data/xpkgs/`), letting each project pick the version it needs without touching the system environment.

## Automatic Installation

On the first `mcpp build` with no toolchain configured, mcpp
installs and persists a default pair for the current host. The choice is
host-aware:

- Linux x86_64 uses `gcc@16.1.0` for the native glibc ABI, so X11, OpenGL, and
  system libraries work out of the box.
- Other Linux architectures use `gcc@15.1.0-musl`, a self-contained static
  toolchain.
- macOS uses `llvm@20.1.7`.
- Windows with a usable MSVC installation uses `llvm@20.1.7` for the MSVC ABI.
  Without usable MSVC, it uses `gcc@16.1.0` with target
  `x86_64-windows-gnu` (MinGW-w64, static by default).

Fully static musl output remains one flag away on a Linux host:
`mcpp build --target x86_64-linux-musl`.

Subsequent builds do not trigger this process again.

> [!TIP]
> In CI, set `MCPP_NO_AUTO_INSTALL=1` to disable only automatic toolchain
> installation. For a fully offline command, use `mcpp --offline` or
> `MCPP_OFFLINE=1`; these also prevent index refreshes and downloads.

## The Identity Model: Toolchain × Target

Two orthogonal axes name everything:

- **toolchain** = `family@version`, family ∈ `gcc | llvm | msvc | emsdk | android-ndk` — *who compiles*
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
mcpp toolchain install llvm 20.1.7          # LLVM/Clang, default on macOS and Windows with usable MSVC
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

### Compiler selection for a build

Five things can name it. They are ranked, and the rank is what makes the two
statements a project can write outrank everything mcpp keeps on its own:

| | source | may mcpp revise it |
|---|---|---|
| 1 | `[target.<triple>] toolchain` in `mcpp.toml` | no |
| 2 | `[toolchain] default` in `mcpp.toml` (or `MCPP_TOOLCHAIN`) | no |
| 3 | `requires = ["mcpp:compiler=<family>"]` from a dependency | — |
| 4 | the target row's pin, when the row's payload supplies the target side | yes |
| 5 | `mcpp toolchain default`, then mcpp's first-run default | yes |

**A dependency may require a compiler family.** A C++ runtime is configured for
one family and records that configuration in the headers it ships, so a package
supplying one states which compiler it was built for. When that requirement
differs from a value at rank 4 or 5 — answers mcpp derived itself — mcpp takes
the required family for that build:

```
$ mcpp build
   Resolving toolchain
    Resolved llvm@22.1.8 → …/xim-x-llvm/22.1.8/bin/clang++
             required by openkal-llvm-runtime@0.1.3 (`requires = ["mcpp:compiler=llvm"]`),
             not your gcc@16.1.0 — this project only
```

**Nothing is written.** Not `~/.mcpp/config.toml`, not the project's
`mcpp.toml`. The requirement is a property of this build, so it applies to this
build; the machine's default stays whatever it was, for every other project.
The version comes from what is already installed — the same resolution
`mcpp toolchain default <family>` performs — and only from the ecosystem's own
pin when nothing of that family is present.

**A compiler the project states is not revised.** At ranks 1–2 the project has
said what it builds with, and a dependency disagreeing is a real contradiction:

```
error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
         compiler          gcc            (16.1.0, payload)
         required          llvm           (required by openkal-llvm-runtime@0.1.3)
       This build's compiler is stated in [toolchain] in mcpp.toml, and a compiler
       the project states outranks one its dependencies ask for.
       Change it to `llvm`, or remove it — with nothing stated, mcpp takes the
       compiler the graph requires and changes no configuration to do it.
```

**Two dependencies requiring different families is an error, not a pick.** One
build has one compiler; resolving by graph-traversal order would decide it by an
order the author neither writes nor can predict, and would satisfy one package
while failing the other inside a header.

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
vocabulary, in four statuses:

| Status | Meaning | Next step |
|---|---|---|
| `installed` | a payload here already produces it | nothing |
| `available` | a payload exists for this host | `mcpp toolchain install` |
| `via dependency graph` | the compiler is here; the target's system is not, and packages can supply it | depend on an implementation of the target's kernel interface and C library |
| `planned` | registered in the vocabulary, not yet shipped | — |

**A target absent from this block cannot be built here at all** — and that
is a narrower statement than it used to be. Until mcpp 2026.8.25.2 the block
listed only what a payload served, so a target whose system comes from a
dependency graph was missing while the same host produced real artefacts for
it. `x86_64-windows-msvc` and `aarch64-macos` remain absent on a Linux host,
correctly: MSVC and the macOS SDK are host-only and no dependency substitutes
for them.

## Windows PE via MinGW-w64 (`x86_64-windows-gnu`, no Visual Studio required)

**This is the Windows default when no Visual Studio is present.** Windows ships
the UCRT runtime DLLs but not the MSVC STL or the Windows SDK — those come with
Visual Studio's "Desktop development with C++" workload. Since llvm on Windows
targets the MSVC ABI and needs both, mcpp checks for a usable MSVC (STL **and**
SDK — half of one is the trap) on first run and falls back here when it finds
none, persisting the choice so later builds are silent. Nothing to install or
configure.

The same check also repairs an existing setup: if a `[toolchain] default` mcpp
chose earlier can no longer work on this machine, it is revised in place, with
a line saying so. An explicit `[toolchain]` in `mcpp.toml` (or a
`[target.X].toolchain`) is never overruled — a project that needs the MSVC ABI
to link vcpkg-built `.lib` files gets an error naming the alternative, not a
silent ABI swap.

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
runs directly under wine). To opt out, set it on the target section —
`linkage` is exact-triple only (§2.7 of [mcpp.toml](04-mcpp-toml.md)), and a
`[build] linkage` key does not exist and is silently ignored:

```toml
[target.x86_64-windows-gnu]
linkage = "dynamic"
```

In a manifest:

```toml
[toolchain]
windows = "gcc@16"            # gcc family on Windows = MinGW-w64
# legacy value "mingw@16.1.0" keeps working
```

Artifact names follow the **target**, and for static libraries the convention
splits on the *env* segment, not on the OS:

| Target | `kind = "lib"` produces |
|---|---|
| `x86_64-windows-gnu` | `libfoo.a` (GNU convention) |
| `x86_64-windows-msvc` | `foo.lib` (MSVC convention) |

Before 2026.8.3.3 a mingw build on a Windows host emitted `foo.lib` — a GNU
archive wearing an MSVC name, which MSVC cannot consume. A script
that globs `*.lib` out of a `windows-gnu` build, it needs to glob `*.a` now.

## Linux ELF from Windows (`x86_64-linux-musl`, no WSL required)

The mirror of the section above: a Windows machine producing a **fully static
Linux binary**, with no WSL, no container, and nothing installed system-wide.

```bash
mcpp build --target x86_64-linux-musl        # from Windows OR Linux
```

The command is spelled *identically* on both hosts, because "cross" is not a
name in mcpp — it is just the relation `host ≠ target`. Which payload serves
the target is resolved automatically: a Linux x86_64 host installs the native
`musl-gcc`; a Windows host installs a **canadian-cross** GCC
(built `x86_64-linux-gnu` → runs on `x86_64-w64-mingw32` → emits
`x86_64-linux-musl`). Both are GCC 16.1.0 and both ship `bits/std.cc`, so
`import std` works the same either way.

The output is a fully static ELF with no `PT_INTERP` — it runs on any Linux
distribution regardless of its libc, which is exactly why musl is the target
that got wired up first:

```console
$ file mcpp
mcpp: ELF 64-bit LSB executable, x86-64, statically linked, stripped
```

`x86_64-linux-gnu` from Windows is **not** supported: a glibc target needs the
`xim:glibc` and `xim:linux-headers` sysroot payloads, which are published for
Linux hosts only. The musl target is self-contained and needs neither.

Cross-arch from Windows (e.g. `aarch64-linux-musl`) is not available either —
the canadian-cross payload is built per host arch. **A macOS host has no
Linux-targeting payload at all**, so no Linux target is reachable from there.

You do not have to memorize any of this: `mcpp toolchain list` shows only what
the current host can actually install, so if a target is missing from the
Targets block, that host genuinely cannot serve it (implemented by
`toolchain::host_can_serve`).

## MSVC (Windows)

An MSVC toolset reaches a build one of two ways, and the **version axis of the
spec** says which:

| Spec | Origin | Compiler resolved |
|---|---|---|
| `msvc@system` (or bare `msvc`) | the machine's own Visual Studio | whatever is installed here |
| `msvc@<toolset>` (e.g. `msvc@14.44.35207`) | an xlings payload mcpp installs | the named toolset, identically on every machine |

They are not alternatives to pick between once — they answer different
questions. `msvc@system` asks *"use what this developer already has"*;
`msvc@14.44.35207` asks *"build this project with exactly this compiler"*.
Pinned toolsets coexist with each other and with a system Visual Studio.

> **`@system` is an MSVC-only spelling.** There is no `gcc@system` or
> `llvm@system`, and that is deliberate rather than an omission: mcpp is built
> on xlings, a user-space OS, and the design drives host dependencies to a
> minimum — a toolchain comes from a payload the manifest names, so every
> machine builds with the same compiler. Windows is the one place where
> refusing to use what is already installed would cost more than it buys:
> Visual Studio is very often present and cannot always be redistributed.
> `<family>@system` for any other family is an error that names both things
> that may have been intended. (The family-less `[toolchain] … = "system"` — the PATH
> compiler — is a separate and deliberate escape hatch, and is unaffected.)

### `[toolchain] … = "system"` — refused

**mcpp builds only with toolchains it manages.** A compiler taken from `PATH` is
not supported, and the configuration is refused rather than warned about:

```
error: [toolchain] linux = "system" is not supported: mcpp builds only with
       toolchains it manages.
       A compiler taken from PATH cannot be identified or reproduced, so
       `import std` availability, the runtime closure and "the same build on
       another machine" all stop being things mcpp can promise.
       Name one instead — mcpp installs it on first use:

         [toolchain]
         linux = "gcc@16.1.0"

       or set a machine default with `mcpp toolchain default gcc@16.1.0`, and
       see `mcpp toolchain list` for what is available.
```

`msvc@system` is **the one exception** and is a different spelling: it names a
*family* whose installation mcpp locates and identifies, on the one platform
where the compiler cannot be redistributed. See the section above.

#### The reason the toolchain and the libraries differ

mcpp's rule about host dependence is not uniform across axes, and the split is
deliberate:

- **mcpp itself, and everything the mcpp ecosystem publishes, depends on no
  host.** Toolchains and payloads come through xlings — the xim index or
  mcpp-index. This is what makes a build reproducible across machines and Linux
  distributions.
- **The toolchain is part of that contract, so it is not the project's to take
  from the host.** Everything mcpp promises — `import std` availability, a
  computable runtime closure, the same build on a teammate's machine and in CI
  — is a statement about a compiler mcpp resolved and can name. A `PATH`
  compiler makes all of it unverifiable, which is why this one is a refusal.
- **The libraries a program links are the program's own business.** A project
  may link a host library or its own `.so`. mcpp says what that costs and names
  the supported route — declare the provider so it resolves from mcpp-index, and
  if the index does not carry it yet, contributing the package is the path — but
  it does not refuse, as long as the result builds and runs. The developer owns
  the artifact and guarantees it.

A build that provably *cannot* run stays an error on either axis: a runtime
closure that cannot be satisfied is refused, because the artifact will not
start. See [binary distribution](12-binary-distribution.md).

### `msvc@system` — the machine's own Visual Studio

mcpp locates and identifies an installed Visual Studio / Build Tools; it never
installs, updates, or removes one.

```bash
mcpp toolchain default msvc
```

mcpp auto-locates it in this order:

1. **`VSINSTALLDIR`** — set by a developer command prompt or by a CI step that
   ran `vcvarsall`. A declared answer, so it outranks the probes below.
2. `vswhere.exe` (including prerelease/Insiders instances)
3. `VS*COMNTOOLS`
4. the standard `Program Files\Microsoft Visual Studio\<year>\<edition>` paths

It then identifies the versions involved and persists the stable spec
`msvc@system`:

```
Detected   msvc 19.44.35211 (VS 2022 BuildTools) (VC tools 14.44.35207)
           cl: C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe
           import std: available (std.ixx)
Default    set to msvc@system (was: llvm@20.1.7)
```

If no Visual Studio is installed, mcpp says so and offers both routes — a
pinned toolset it can install, or the Visual Studio Installer /
`winget install Microsoft.VisualStudio.2022.BuildTools`.

`mcpp toolchain list` shows the detected MSVC in a separate `System:` section,
and `mcpp self doctor` reports its status on Windows. In a manifest:

```toml
[toolchain]
windows = "msvc@system"
```

### `msvc@<toolset>` — a toolset mcpp installs and pins

```bash
mcpp toolchain list --available msvc     # what can be pinned
mcpp toolchain install msvc 14.44.35207
```

This works like `gcc@16.1.0` in every respect: the payload is downloaded into
mcpp's own store, several toolsets coexist, `mcpp toolchain remove
msvc@<toolset>` uninstalls one, and a manifest that names one gets it
installed automatically on first build.

```toml
[toolchain]
windows = "msvc@14.44.35207"
```

**The version is the toolset directory name** (`14.44.35207` — what
`VC\Tools\MSVC\` is named and what `-vcvars_ver` takes), *not* the cl banner
version (`19.44.35211`) and not the product year. Nothing needs to be
installed on the machine: the payload brings the compiler, the STL, and — via
its `xim:windows-sdk` dependency — the ucrt/um headers and libraries.

> **Changed:** `msvc@19.44` used to mean "use the system MSVC and verify its
> banner starts with 19.44", which was checked by `mcpp toolchain default` and
> silently ignored by builds. The version axis now names a toolset everywhere.
> A `19.x` spelling gets an error naming both replacements — `msvc@system` or
> the toolset version that machine actually has.

### Native cl.exe builds

Since 0.0.90 these work on both origins: mcpp synthesizes the INCLUDE/LIB
environment from the VC tools + Windows SDK (no `vcvarsall` involved), stages
`std.ixx`/`std.compat.ixx` as `.ifc` BMIs, compiles `.cppm` module units via
`/interface /TP /ifcOutput`, scans with `/scanDependencies`, and links with
`link.exe`/`lib.exe` through response files.

**The Windows SDK follows the origin**, because the two origins answer
different questions and so must the SDK:

| origin | SDK selection |
|---|---|
| `msvc@<toolset>` | the `xim:windows-sdk` payload installed **with that toolset**, in mcpp's own store. `WindowsSdkDir` / `WindowsSdkVersion` in the environment are **ignored**, and mcpp prints a `note:` saying so. |
| `msvc@system` | **`WindowsSdkDir`** (+ `WindowsSdkVersion`) if declared, then `C:\Program Files (x86)\Windows Kits\10`. |

The asymmetry is the point. A pinned toolset is a promise that two machines
compile the same source against the same headers; an environment variable that
can quietly redirect it turns the pin into a preference. A machine's own SDK,
on the other hand, can only be found by looking, and there a declared answer
outranks a scan — the same precedence `VSINSTALLDIR` has over `vswhere`.

If a pinned toolset has no SDK payload beside it (an older install, say), mcpp
falls back to the machine's SDK rather than failing — and says so, because that
build is no longer reproducible and nothing else would record it.

A root only counts as an SDK when it has **both** halves — `Include\<v>\ucrt\
corecrt.h` *and* `Lib\<v>\um\<arch>\kernel32.lib`. A root with headers and no
import libraries is skipped rather than selected, so a partially unpacked
payload cannot outrank the machine's complete SDK and turn into
`LNK1104: cannot open file 'kernel32.lib'` at the very end of a build.

The resolved SDK version is part of the build's **runtime identity**
(`ucrt@10.0.26100.0`) and therefore of the fingerprint that keys the build
cache: changing SDK changes the cache key, exactly as changing compiler does.
It is a **compatibility floor declaration**, not a payload binding like
`glibc@2.39` on Linux — `ucrtbase.dll` is a Windows component and mcpp neither
ships nor substitutes it.

**CRT model.** `/MD` (host-coupled) by default; `/MT` when either

```toml
[target.x86_64-windows-msvc]
linkage     = "static"           # the libc axis — TARGET section, or `--static`

[build]
cxx_runtime = "self-contained"   # the C++ runtime axis
```

is written down. Note which section each one lives in: `linkage` is
exact-triple only and **there is no `[build] linkage` key** — writing one gets
an "unsupported key (ignored)" warning and no static CRT. (This page said
exactly that a few sections up, and then showed the wrong form here.) On the MSVC ABI these are one physical switch — `/MT` links
the C and C++ runtimes out of the same library — so both spellings select it
and mean the same thing. It is a **whole-project** property: one `std` module
is built per project and cl bakes `_MSVC_MT`/`_MSVC_MD` into it, so a
per-role override (`cxx_runtime = { tests = … }`) is refused with a message
saying so rather than producing a module mismatch inside the ucrt headers.

## SDK Toolchains (`emsdk`, `android-ndk`)

Two of the five toolchain spellings name an **SDK** rather than a bare
compiler: `emsdk` and `android-ndk`. Their compiler *is* clang -- so they are
not a separate compiler family, and mcpp does not pretend they are -- but the
archive brings its own sysroot, its own C library and, for both of these, its
own generated `std` module surface. That difference is what the rest of this
section is about.

### Nothing has to be declared

A target row names its own payload, and that pin is the default. Neither of
these needs a line in `mcpp.toml`:

```bash
mcpp build --target wasm32-emscripten     # resolves emsdk@6.0.9
mcpp build --target aarch64-linux-android # resolves android-ndk@30.0.16248370
```

The payload is **installed on demand** the first time a target needs it, the
same way a gcc or llvm payload is. `mcpp toolchain list` shows the pin beside
the row, and the build reports which archive answered:

```
Resolved emsdk@6.0.9 → wasm32-emscripten → …/xim-x-emsdk/6.0.9/emscripten/em++
Resolved android-ndk@30.0.16248370 → aarch64-linux-android → …/prebuilt/linux-x86_64/bin/clang++
```

### Declaring one anyway

The ordinary per-target key works, and naming the row's own payload is always
accepted:

```toml
[target.aarch64-linux-android]
toolchain = "android-ndk@30.0.16248370"

[target.wasm32-emscripten]
toolchain = "emsdk@6.0.9"
```

Use it to pin a version across machines, or to opt into a payload newer than
the row's convention. The **version** is free -- anything the index publishes
resolves -- so this is how a project moves ahead of, or stays behind, the
default.

### What cannot be overridden, and why

For these rows the pin is a **capability** rather than a convention: it is not
mcpp's preference among several payloads that could serve the target, it is the
only thing that can. So the payload NAME is fixed while the version is open:

```toml
[target.aarch64-linux-android]
toolchain = "llvm@22.1.8"        # refused
```

```
error: target 'aarch64-linux-android' cannot be emitted by 'llvm@22.1.8'.
       An Android target needs bionic, not just an aarch64 or x86_64 back end:
       its headers, its per-API-level stubs and its loader path are inside the
       NDK, and no package adds them to another compiler.
```

The refusal is not about code generation. A stock clang emits aarch64 ELF
perfectly well; what it cannot supply is the SYSTEM. Saying so at the
declaration is better than resolving llvm and failing deep inside the build,
which is what happened before this gate existed -- first `'__config' file not
found`, then `Unversioned target triples are not supported!` from bionic's own
header, neither of them naming the toolchain that could not serve the row.

`wasm32-emscripten` refuses on the same rule with its own sentence: nothing but
Emscripten emits WebAssembly.

### What belongs to the project instead

The toolchain is the SDK's; the **deployment floor** is the project's, and it
has its own key per platform -- see
[04 — mcpp.toml](04-mcpp-toml.md) §2.7.3:

```toml
[target.aarch64-linux-android]
min_api_level = 24               # Android
```

```toml
[package]
macos_deployment_target = "14.0" # Apple
```

One NDK serves a range of API levels, so the level is a project decision and
naming `android-ndk@<version>` does not pin one. Left out, mcpp reads the floor
the NDK itself declares in `meta/platforms.json`.

### Running what they produce

Neither the emulator nor a device is part of the toolchain axis. `mcpp run`
executes a wasm module directly, because Emscripten's output is a program
`node` can run. For a target whose artifact runs elsewhere, the `runner` key is
an argv prefix and the session belongs to a package rather than to the engine:

```toml
[target.x86_64-linux-android]
runner = ["adb-run"]             # a program from xim:android-platform-tools
```

## Project-Level Version Pinning

If a project needs to pin a specific version rather than rely on the global default, declare it in the project's `mcpp.toml`:

```toml
[toolchain]
default = "gcc@16.1.0"
linux   = "gcc@16.1.0"
macos   = "llvm@20.1.7"
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

### A convention may be overridden. A capability may not.

The pin on a **hosted** row answers *which payload supplies this target's C
library*, so a project that supplies one itself may name any compiler — that is
the escape hatch the whole openkal ecosystem is built on. What it may not do is
name a different compiler and supply nothing:

```
$ mcpp build --target x86_64-linux-musl        # [toolchain] default = "llvm@…"
error: target 'x86_64-linux-musl' takes its C library from the 'gcc@16.1.0'
       payload, and 'llvm@22.1.8' has none here.
```

Two rows answer a different question, and their pin cannot be overridden at all:

| row | reason |
|---|---|
| every `*-none-elf` | no per-host cross payload exists; clang and lld are cross-compilers by construction and gcc is not |
| `x86_64-windows-musl` | no gcc payload emits a PE with a musl C library — the mingw payload emits PE with the MinGW CRT, which is the separate `-gnu` row |

```
$ mcpp build --target riscv64-none-elf         # [toolchain] default = "gcc@…"
error: target 'riscv64-none-elf' cannot be emitted by 'gcc@16.1.0'.
```

**Both refusals are decided where the decision is made**, not left to the
compiler. Before 2026.8.26.1 the first ran the whole build and died at the link
on `crtbeginT.o (bare name)`, and the second produced
`g++: error: unrecognized argument in option '-mabi=lp64d'` — a message about an
option, for a decision made a hundred lines earlier.

A program classifying these reads `data.reason` from
`mcpp why toolchain --format json` (`convention-unreplaced` / `capability-pin`)
rather than the sentence — see [chapter 11](50-machine-output.md).

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
| `MCPP_OFFLINE=1` | Never touch the network; equivalent to global `--offline` |
| `MCPP_NO_COLOR=1` / `NO_COLOR=1` | Disable colored output |
| `MCPP_LOG_LEVEL=debug\|info\|warn\|error\|off` | Log level |

When `MCPP_HOME` is not set explicitly, mcpp locates the sandbox automatically based on the parent directory of the binary (after a release tarball is extracted to `~/.mcpp/`, `~/.mcpp/` is the home), so the release build runs without any environment variable configuration.


## ABI Capability Enforcement

A dependency can declare an `abi:<name>` capability (for example, `compat.glfw` declares `abi:glibc`). When the resolved toolchain's ABI does not satisfy any dependency's abi requirement, the build **fails early** with a suggested fix (for example, a musl-static toolchain encountering an abi:glibc dependency), replacing deeper link/header errors. Inspect with: `mcpp why toolchain`.

## Known Toolchain Hazard: Operator Templates in Module Interfaces (Clang 20+)

A module that exports **replacement operator templates** can poison name
lookup for that operator in every importer when compiled by Clang 20 or 22:
any translation unit that `import`s the module and uses that operator — **on
any type at all** — crashes the frontend (SIGSEGV). GCC 16 and Clang 18 are
unaffected, so this is a regression somewhere between Clang 18 and 20.

This bites the module-package pattern directly. Wrapping an upstream header
whose operators are `static inline` templates, and mirroring their signatures
with a trivially-true constraint (the standard mixed-TU subsumption recipe),
is exactly how it is reached.

**The rule of thumb:** every template parameter should be pinned by the
**first** function argument. Shapes that break this are the poisonous ones:

```cpp
// Poisonous — `n` and `l` are not determined by argument 1
template<typename T, int m, int n, int l>
Matx<T, m, n> operator*(const Matx<T, m, l>& a, const Matx<T, l, n>& b);

// Poisonous — second typename appears only in argument 2
template<typename T1, typename T2, int n>
Vec<T1, n>& operator+=(Vec<T1, n>& a, const Vec<T2, n>& b);

// Fine — every parameter is pinned by argument 1
template<typename T, int m, int n>
Matx<T, m, n> operator+(const Matx<T, m, n>& a, const Matx<T, m, n>& b);
```

The crash is **name-keyed**: one poisoned `operator*` declaration makes every
`x * y` in every importer crash, for entirely unrelated types. The function
body is irrelevant.

**The workaround** is to deduce whole operand types and constrain them,
rather than destructuring them in the parameter list. It stays
call-compatible, and mixed-TU semantics survive because the upstream
exact-pattern `static inline` remains more specialized and still wins there:

```cpp
template<typename MA, typename MB>
    requires pick<typename MA::value_type>
          && __is_same(MA, typename MA::mat_type)
          && __is_same(MB, Matx<typename MB::value_type,
                               (int)MA::rows, (int)MA::cols>)
inline MA& operator+=(MA& a, const MB& b);
```

Tracked as [mcpp#256](https://github.com/mcpp-community/mcpp/issues/256).
`tests/e2e/150_clang_module_operator_template.sh` is a canary over the
bundled LLVM toolchains, so a future Clang bump that fixes — or re-breaks —
this becomes visible instead of silently changing what packages can express.

## The C++ runtime contract (`cxx_runtime`)

`cxx_runtime` states what the produced artifact promises about the machine that
runs it. It is a **distribution** property, not a build one — it describes the
runtime dependency set, and the flags that deliver it differ per platform.

> **A target with no C++ in it has no C++ runtime contract to honour.** mcpp
> links such a target with the C driver and leaves the C++ runtime flags off the
> line entirely, so a pure-C shared library does not acquire a dependency on
> `libstdc++`/`libc++` it has no use for. One C++ translation unit anywhere in
> the target puts the whole target back on the C++ driver. This is derived from
> the sources — there is no key for it.

```toml
[build]
cxx_runtime = "self-contained"          # applies to every target (the default)

# or, per role:
[build.cxx_runtime]
default = "self-contained"              # executables
tests   = "host-coupled"                # test binaries never leave this machine
shared  = "self-contained"              # shared libraries (see below — the
                                        # default differs by target format)

# or, per target triple — beside `linkage`, which is the same axis:
[target.x86_64-linux-gnu]
cxx_runtime = "host-coupled"            # e.g. this build is for a distro package
```

| value | the artifact needs, at run time | typical use |
|---|---|---|
| `self-contained` (default) | no C++ runtime outside itself | shipping a binary |
| `toolchain-coupled` | the C++ runtime of the toolchain mcpp installed | local iteration |
| `host-coupled` | whatever the driver resolves by default (the system runtime) | distro packaging, `dlopen` plugins that must share a runtime with their host |

**Self-contained by default (portable by default)**: on macOS this statically
links LLVM's bundled libc++/libc++abi — the system libc++ would otherwise pin the
runnable version to the build machine's OS (older systems lack newer symbols, e.g.
the support symbols behind `std::print`), and only static linking can truly deliver
the `macos_deployment_target` floor. On Linux/MinGW it is `-static-libstdc++` (GCC)
or the whole-link `-static` (MinGW); on a Linux clang/libc++ toolchain it links
libc++.a/libc++abi.a/libunwind.a explicitly. A lower macOS floor (11–13) requires a
self-built libc++ archive (already verified to work, a data-level switch, available
on request).

**Shared libraries are the one role whose default depends on the target format**,
because the hazard does. A `.so`/`.dylib`/`.dll` is not a small executable — it is
loaded *into* a process that already has a C++ runtime.

| target | default for `kind = "shared"` | reason |
|---|---|---|
| ELF (Linux, …) | `toolchain-coupled` | ELF has one global symbol namespace and the first definition loaded wins. A `.so` that statically embedded libstdc++ **exports** it, and the executable linking that library binds *its* `std::` references there — its own `self-contained` contract silently becomes a no-op, and its C++ runtime is whichever build of that library happens to load. |
| Mach-O | `self-contained` | the mechanism there is already `-load_hidden`, i.e. hidden visibility, so dyld never unifies those symbols; and toolchain-coupled is not available on macOS at all (see the note below). |
| PE (Windows) | `self-contained` | PE has no global symbol namespace — imports resolve per-DLL by name, so a DLL's private runtime cannot be picked up by anything else. |

Setting `shared = "self-contained"` on ELF is supported and does exactly what it
says: the library embeds the runtime. mcpp additionally passes
`-Wl,--exclude-libs` for the standard-library archives, so the embedded copy stays
out of the library's dynamic symbol table and cannot be picked up by anything that
links it. Template instantiations emitted by the consuming code (weak/COMDAT `std::string`
symbols and the like) are still exported — that is the intended C++ ABI behaviour
and is not the leak this guards against.

A project-wide `cxx_runtime = "…"` (or `static_stdlib = false`) applies to shared
libraries too: a human said what the whole project promises. The format-specific
default applies only when nobody said anything.

`static_stdlib` is the older spelling and still works: `true` means
`self-contained`, `false` means `host-coupled`. An explicit `cxx_runtime` wins.

**A contract that cannot be honored is reported, never silently downgraded.** If a
toolchain ships no `libc++.a`, or a contract has no mechanism on that platform,
the build prints what it fell back to instead of quietly producing a different
artifact than the manifest asked for.

### On the MSVC runtime

The CRT model is the mechanism here, and it is a **whole-project** switch: cl
bakes `_MSVC_MT`/`_MSVC_MD` into the one `std` module a project builds, so a
per-role contract that disagrees with the project's cannot be honoured and is
reported rather than ignored.

| value | meaning on MSVC |
|---|---|
| `self-contained` | `/MT` — the static CRT. `linkage = "static"` selects the same thing from the libc axis. |
| `host-coupled` (default under `/MD`) | the target provides `vcruntime140.dll` / `msvcp140.dll` — i.e. Visual Studio or the redistributable is installed there. |
| `toolchain-coupled` | the toolset's **own** copy of those DLLs travels with the artifact. |

`toolchain-coupled` is worth spelling out, because the obvious reading is
wrong. `ucrtbase.dll` *is* a Windows component (since Windows 10) and mcpp
never ships it. `vcruntime140.dll` and `msvcp140.dll` are **not**: every MSVC
toolset carries them under `VC\Redist\MSVC\<version>\<arch>\`, exactly the
way a gcc payload carries `libstdc++.so`. Under this contract mcpp stages them
beside the artifact — which is what makes a default `/MD` build runnable on a
machine that has only the pinned toolset and no Visual Studio at all.

The debug CRT (`vcruntime140d.dll` and friends, under `debug_nonredist\`) is
never staged: it may not be redistributed.

> **Upgrading from 2026.8.15 or earlier?** This key used to be **inert** on the
> MSVC ABI — it reported `not implemented for the MSVC runtime yet` and every
> value fell back to `/MD`. Since 2026.8.16 it is honoured, so a manifest that
> carries `cxx_runtime = "self-contained"` from that era **changes CRT model on
> upgrade**, from `/MD` to `/MT`. It is not a stricter version of the same
> model, and the switch is silent because the value was always valid. A project
> that set it while the key did nothing should re-confirm the intended value.

Combining it with `/MT` is a contradiction rather than a missing feature — a
static CRT leaves no DLL to couple to — so it is reported and resolved to
`self-contained`. `mcpp pack` enforces the other half: a mode that bundles
nothing (`--mode system`, `--mode static`) cannot deliver `toolchain-coupled`
and refuses.

**Scope.** The contract governs the C++ runtime only. Static **libc** is a separate
axis (`linkage = "static"` / `--static`, e.g. a musl target), and the deployment
floor is a third — `macos_deployment_target` in `[package]` for Apple targets,
`min_api_level` under `[target.<triple>]` for Android. Both feed one parameter
of `llvm_triple()` and one slot in the build fingerprint, because a target is
either Apple or Android and the two answer the same question. Also, `host-coupled` means mcpp adds
nothing to embed a C++ runtime; it does not strip the toolchain rpath the link
carries for other reasons, so on ELF such an artifact may still find the
toolchain's libraries first.

> **macOS + `self-contained` and static initialization order.** Mach-O has no
> priority-ordered initializer section and libc++'s `<iostream>` carries no
> `ios_base::Init` guard of its own (unlike libstdc++ and the MSVC STL), so a
> stream initializer pulled out of `libc++.a` would otherwise run *after* the
> program's own global constructors — a global whose constructor touches
> `std::cout` would read an unconstructed stream and crash at process start. mcpp
> links a tiny generated object first to force the streams up; nothing is required
> of the calling code. See mcpp-community/mcpp#336.

`defines` takes **bare** macro names (no `-D`) and desugars each entry to `-D<x>` on
both the C and C++ compile channels. It reaches every TU in the package — module
interface units included — so it also reaches the compiler's own P1689 module scan.

> **It does not make a macro-guarded `import` acceptable.** mcpp runs its own
> lexical pre-scan before the compiler ever sees the file, and that scanner
> rejects an `import` inside **any** `#if` / `#ifdef` block without evaluating the
> condition:
>
> ```
> error: import statement inside conditional preprocessor block (forbidden in M1)
> ```
>
> So a `#ifdef FOO` / `import bar;` pair fails even when `FOO` is in `defines`.
> Put the conditional around an `#include` in the global module fragment instead.
> Tracked as mcpp-community/mcpp#421. Assembly units pick it up too. It is a
build input like any other, so `[target.'cfg(...)'.build]` can carry it:

```toml
[build]
defines = ["APP_NAME=\"demo\""]

[target.'cfg(windows)'.build]
defines = ["USE_WIN32", "WINVER=0x0A00"]
```

Picking the right axis:

| You want the macro on… | Use |
|---|---|
| every TU of this package | `[build].defines` (here) |
| one binary's own entry source only | `[targets.<name>].defines` |
| a specific set of files | `[build].flags` with a `glob` + `defines` |
| every TU **and** every consumer's TUs | `[features.<name>].defines` (an interface contribution) |

`[build].defines` is private to the package: it does not propagate to consumers.

Unsupported keys under `[build]` are reported as a warning (an error under
`--strict`) rather than silently ignored.

Do not configure the C++ standard via `build.cxxflags = ["-std=..."]`. Instead use:

```toml
[package]
standard = "c++26"
```

mcpp applies the same standard to ordinary C++ compilation, module scanning,
`compile_commands.json`, and the standard library BMI build for `import std`.

**glob exclusion** (`!` prefix, mcpp 0.0.4+):

```toml
[build]
sources = [
    "src/**/*.cpp",
    "!src/**/*_test.cpp",       # Exclude test files
    "!src/**/*_fuzzer.cpp",     # Exclude fuzzers
]
```

**Per-glob flags** (mcpp 0.0.95+): `[build] flags` is an ordered array of
inline tables attaching extra compile flags to exactly the sources a glob
matches — the tool for SIMD dispatch TUs and vendored-code warning isolation:

```toml
[build]
flags = [
  { glob = "third_party/**",            cflags = ["-w"], cxxflags = ["-w"] },
  { glob = "src/simd/**/*.avx2.cpp",    cxxflags = ["-mavx2"], defines = ["HAVE_AVX2"] },
  { glob = "src/x86/**/*.asm",          asmflags = ["-DPREFIX"] },
]
```

Keys per entry: `glob` (package-root-relative, required) plus `cflags` /
`cxxflags` / `asmflags` / `defines` (no `ldflags` — linking has no per-TU
scope). Declaration order is application order: a later entry's flags land
later on the command line, so with GNU "last flag wins" a narrower glob
placed after a broader one overrides it. All matching entries apply. These
are private build flags — they never propagate to consumers. A glob that
matches no source file prints a warning (a typo'd glob must not silently do
nothing).

**Generated files** (mcpp 0.0.95+): `[generated_files]` maps a relative path
to file contents (TOML multiline strings supported). Entries are written
into the project tree before source globs expand — the same mechanism index
descriptors use to synthesize module wrappers — and the content enters the
fingerprint, so editing it rebuilds:

```toml
[generated_files]
"src/gen/wrap.cppm" = """
module;
#include <vendored.h>
export module wrap;
"""
```

Paths must stay inside the project root (`..` / absolute paths are parse
errors).

**Assembly sources** (mcpp 0.0.95+): `.S`/`.s` (GAS — preprocessed by the C
driver, covers ARM and AT&T-syntax x86) and `.asm` (NASM — Intel-syntax x86)
are first-class sources: default-globbed, fingerprinted, built incrementally
in parallel, and linked like any other object. The NASM output format is
derived from the target triple (`elf64`/`win64`/`macho64`/... — cross builds
just work), and `nasm` itself is resolved lazily only when `.asm` units exist:
`PATH` first, then the mcpp sandbox, then `xlings install nasm`; if none
yields nasm ≥ 2.16 the build **fails hard** (assembly is never silently
skipped). Limits: `.asm` targets x86 only (hard error elsewhere — gate the
files off other targets), `.S` is unavailable on the MSVC toolchain, and
`.asm` means NASM syntax (MASM sources should be `!`-excluded).

## Current limitations

- **A macOS host has no Linux-targeting payload at all**, so no Linux target is
  reachable from there. This is a publishing boundary rather than an engine one.
- An explicit `[toolchain]` in the manifest, or a `[target.X].toolchain`, is
  **never overruled**. A project that pins one and then meets a target the pin
  cannot serve is refused rather than silently given another compiler.
