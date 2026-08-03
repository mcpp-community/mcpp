# 03 — Toolchain Management

> mcpp maintains an independent toolchain sandbox, fully isolated from the system PATH.

## Motivation

C++23 modules are fairly sensitive to compiler versions, and different releases of GCC / Clang differ noticeably in how they handle module semantics. The versions shipped by system package managers tend to lag behind, and keeping multiple versions side by side carries a maintenance burden. mcpp installs all toolchains into a single sandbox directory (`~/.mcpp/registry/data/xpkgs/`), letting each project pick the version it needs without touching the system environment.

## Automatic Installation

The first time you run `mcpp build`, if no toolchain is configured yet, mcpp
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
`linkage` is exact-triple only (§2.7 of [mcpp.toml](05-mcpp-toml.md)), and a
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
archive wearing an MSVC name, which MSVC cannot consume. If you have a script
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
files. `[target.x86_64-windows-msvc] linkage = "static"` (or `mcpp build
--static`) selects the `/MT` CRT — not `[build] linkage`, which is not a key.
A missing Windows
SDK fails the build with installation guidance (`mcpp self doctor` reports
SDK status).

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

## HarmonyOS / OpenHarmony

```bash
export OHOS_NDK_HOME=/path/to/ohos-sdk/linux/native
mcpp build --target aarch64-linux-ohos
```

HarmonyOS is the one target where mcpp needs something it cannot ship. Every
other target is served by a toolchain mcpp downloads; this one additionally
needs the **platform SDK**, a ~2.5 GB vendor archive under its own licence.
Point `OHOS_NDK_HOME` at the unpacked `native` directory and everything else
is the usual `--target` flow.

**mcpp uses the SDK as a sysroot, not as a toolchain.** The compiler stays
mcpp's own LLVM. That is not a preference — the SDK's bundled clang is
**15.0.4 even in SDK 6.1 (API 23)**, four major versions below what C++20
modules need (`-fmodule-output` arrived in clang 16), and it has been that
version for years. So mcpp retargets its own clang with `--target=` and takes
the libc, C++ standard library, CRT objects and compiler-rt builtins from the
SDK. GCC is not an option here at all: it has no `ohos` target.

mcpp finds the SDK through, in order: `$OHOS_NDK_HOME`, `$OHOS_SDK_NATIVE`
(what `openharmony-rs/setup-ohos-sdk` exports), `$OHOS_SDK_HOME/native`,
`$OHOS_SDK_HOME/<api>/native`, `$DEVECO_SDK_HOME/…`, then `~/ohos-sdk/native`
and `/opt/ohos-sdk/native`.

Targets: `aarch64-linux-ohos` (verified), `x86_64-linux-ohos` and
`arm-linux-ohos` (registered, not yet verified). Default linkage is static —
the OHOS libc is a musl fork, and as with mcpp's other musl targets a static
artifact is the one that runs anywhere without a matching loader.

`cfg()` sees HarmonyOS as **`env = "ohos"` on `os = "linux"`**, matching
upstream LLVM's own model. The kernel really is Linux, so a package that
gates on `cfg(os = "linux")` or `cfg(family = "unix")` keeps working; only
code that needs HarmonyOS *specifically* has to say so:

```toml
[target.'cfg(env = "ohos")'.build]
defines = ["USE_OHOS_HILOG"]
```

In C++, the SDK's `__OHOS__` macro is defined by mcpp's clang too.

### `import std` on HarmonyOS

Against a **stock SDK**, named modules work and `import std` does not — the
SDK ships libc++ 15.0.4, which has no `std` module at all. mcpp says so
during resolution rather than failing later:

```
Note this target's libc++ ships no `std` module — `import std;` is
unavailable here; named modules and #include both work.
```

This is a missing **payload**, not a missing capability. Build libc++ from
LLVM sources for the target and `import std` works exactly as it does
everywhere else:

```bash
git clone --depth 1 --branch llvmorg-20.1.7 https://github.com/llvm/llvm-project
CLANGXX=~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/bin/clang++
RES=$OHOS_NDK_HOME/llvm/lib/clang/15.0.4

cmake -G Ninja -S llvm-project/runtimes -B build-ohos-libcxx \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/ohos-libcxx \
  -DCMAKE_C_COMPILER="${CLANGXX%++}" -DCMAKE_CXX_COMPILER="$CLANGXX" \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-ohos \
  -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-ohos \
  -DCMAKE_SYSROOT="$OHOS_NDK_HOME/sysroot" \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_FLAGS="--no-default-config" -DCMAKE_CXX_FLAGS="--no-default-config" \
  -DCMAKE_EXE_LINKER_FLAGS="-resource-dir=$RES -fuse-ld=lld" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_HAS_MUSL_LIBC=ON \
  -DLIBCXX_INSTALL_MODULES=ON \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
  -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF
ninja -C build-ohos-libcxx install

# CMake's ASM language does not inherit CMAKE_CXX_COMPILER_TARGET, so the
# installed libunwind.a holds HOST objects and lld rejects it. Use the
# platform's own unwinder — which is what the SDK lib dir provides.
rm -f ~/ohos-libcxx/lib/libunwind.a

export MCPP_OHOS_LIBCXX=~/ohos-libcxx
mcpp build --target aarch64-linux-ohos      # import std now works
```

Build the overlay with the **same** LLVM version the target pin resolves to.
A libc++ built by one clang and consumed by another is a version skew that
works right up until it does not.

`MCPP_OHOS_LIBCXX_AARCH64_LINUX_OHOS` (target-suffixed, `-` → `_`, upper
case) overrides `MCPP_OHOS_LIBCXX` when one machine holds overlays for
several targets.

### What is verified, and what is not

CI cross-builds both tiers and **runs** the artifacts under `qemu-aarch64`
(`.github/workflows/ci-harmonyos.yml`, `tests/e2e/103_…` and `104_…`). That
proves the artifact targets the right machine and really executes — the same
claim the `aarch64-linux-musl` row makes.

It does **not** cover the `.hnp`/`.hap` packaging path, linking the
platform's own NDK libraries (`libace_napi.z.so` and friends), or anything
requiring a device or the emulator. Those remain out of scope; see
`.agents/docs/2026-08-04-harmonyos-target-design.md`.

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
is exactly how you hit it.

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
