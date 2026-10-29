# 35 — Platforms

**Reader:** someone building for a target a platform package configures (an ok
image, `<arch>-openkal`), changing part of that configuration, or writing a
platform package for a class of targets.

**The question this chapter answers:** how a class of targets is configured as
one package, how a build selects it, and how a project overrides or replaces it.

**Not here:** the target-side layers a platform names, which are
[22 — The Target Side](22-target-side.md); openkal's model and its hosted
cross-compilation, which are [24 — Cross-Compilation Over openkal](24-openkal-cross.md);
packaging a program, which is [10 — Packaging an Application for Release](10-pack-and-release.md);
the exact rules, which are [SPEC-013](specs/platforms.md).

Before: [34 — Authoring a Board-Support Package](34-authoring-a-bsp.md).
After: [40 — Bare-Metal and Freestanding Targets](40-baremetal.md).

A **platform** is one versioned package that states everything a class of
targets needs: the toolchain, the package that provides each target-side layer,
the tools the build runs on this machine, the compile and link conventions, the
name and seal of an artifact, and the runner. A project states none of it. The
target table selects `mcpplibs.openkal-platform` for `x86_64-openkal` and
`aarch64-openkal` (2026.10.27.1+), which is what built-in support for those two
targets is.

## The shortest run

A project that names only its sources:

```toml
[package]
name    = "app"
version = "0.1.0"
```

```sh
mcpp run --target x86_64-openkal
```

```
    Resolved llvm@23.1.3 → x86_64-openkal → …/xim-x-llvm/23.1.3/bin/clang++
             the platform's (openkal-platform 0.18.0), replacing your gcc@16.1.0 — override with `[target.x86_64-openkal] toolchain`
      Target x86_64-openkal → x86_64-unknown-none-elf
             compiler-runtime  compiler-rt    (openkal-llvm-runtime@0.16.6, platform)
             c-abi             musl           (openkal-musl@0.22.6, platform)
             c++-abi           libc++         (openkal-llvm-runtime@0.16.6, platform)
    Platform openkal-platform 0.18.0 (built-in)
             tools             okld, okrun (openkal-tools@0.1.3, platform)
             runner            okrun (platform)
             link              lld, start ok_main (platform)
   Compiling app v0.1.0 (.)
     Running `okrun … target/x86_64-openkal/…/bin/app.oke`
```

Every value is reported with its source. `mcpp test`, `mcpp pack` and
`--target aarch64-openkal` need nothing more; an image of another architecture
than this machine's runs through an execution host
([24](24-openkal-cross.md)). `mcpp.lock` records the packages
the platform added with `origin = "platform"`, and each entry's configurations
in `targets` (absent for this machine's native configuration alone), so a build
for this machine and one for an ok image leave each other's entries in place.

## What a platform states

| Group | Keys | Effect |
|---|---|---|
| the targets | `targets`, `toolchain`, `sysroot` | which triples it serves, the toolchain they use, a prebuilt target base |
| the layers | `[platform.layers]` | the package for `c-abi`, `c++-abi`, `compiler-runtime`, `kernel-abi`, or `false` |
| the tools | `[platform.tools]` | programs built for this machine, or found (`{ program = "<name>" }`) |
| the compile | `[platform.build]`, `[platform.build.'cfg(…)']` | flags for every unit of the target |
| the link | `[platform.link]` | `driver`, `flags`, `start`, `script`, `inputs`, the names `exe` and `shared`, the `seal` steps |
| the run | `runner`, `runners` | how `mcpp run` and `mcpp test` start an artifact, and named device actions |
| the carrier | `carrier` | the program `mcpp pack` builds of a loaded artifact for a system: a self-contained tree's runner, a single file; a table or a list of tables, each serving the `<os>-<arch>` pairs of its `hosts` |
| where the runner exists | `runner.hosts` | the `<os>-<arch>` pairs the runner's program exists for: where a package that needs it runs (`runs_on.with`, [10](10-pack-and-release.md)) |
| the code | `plugin` | a build program run in the platform phase, for what depends on the graph |
| the group | `[platform.versions]` | one version of each package of a family |
| the host part | `[platform.hosted]` | what a host triple takes when it selects the platform |

## Overriding part of a platform

A project overrides a field under `[target.<selector>]` with the platform's own
key name. `cfg(platform = "openkal")` matches the platform whatever the
architecture:

```toml
[target.'cfg(platform = "openkal")'.layers]
c-abi = { path = "../openkal-musl" }         # a working tree in place of the release

[target.'cfg(platform = "openkal")'.tools]
okld = { path = "../openkal-image/tools" }

[target.'cfg(platform = "openkal")']
runner = ["my-okrun", "{}"]
```

| Field | Rule |
|---|---|
| compile flags, `link.flags` | appended after the platform's; a convention cannot be removed |
| `link.inputs`, `link.seal` | replaced whole |
| `layers`, `tools`, `runners` | replaced key by key; `<layer> = false` leaves the layer out |
| `runner` | the argv is replaced; `host` and `exclusive` are kept unless stated |
| every other field | replaced |

The same tables are accepted in `~/.mcpp/config.toml` for one machine. A layer
a package of the graph already provides is left to that package and reported
`graph`. The package of the C++ run-time also provides the compiler run-time, so
`"c++-abi" = false` takes effect only with `compiler-runtime = false` beside it.

## Selecting a platform, or none

The first of these that answers selects the target's platform:

| Source | Spelling |
|---|---|
| the command line | `--platform openkal-platform@0.18.0`, `--platform path:../openkal-platform`, `--platform none` |
| the environment | `MCPP_TARGET_X86_64_OPENKAL_PLATFORM=…` |
| the project | `[target.<selector>] platform = "openkal-platform@0.18.0"`, or `{ path = "…" }` |
| this machine | the same key in `~/.mcpp/config.toml` |
| the target table | the platform column (`mcpp toolchain list`) |

`platform` is not accepted under a `cfg(platform = …)` selector. `none` builds
with the engine's conventions for the target and nothing else.

## The host graph's platform

Build programs and the tools the build runs are compiled for this machine by
its own conventions. `[host] platform` selects a platform for them instead:

```toml
[host]
platform = "openkal-platform@0.18.0"
```

```
        Host platform openkal-platform 0.18.0 (mcpp.toml:2): build programs are x86_64-openkal images run by okrun
```

Every build program then compiles as an ok image of this machine's
architecture, with one toolchain on Linux, macOS and Windows, and runs through
the platform's runner. The platform's own tools are still built natively.
`--host-platform` and `MCPP_HOST_PLATFORM` select it for one run, and
`[host]` in `~/.mcpp/config.toml` for one machine.

## The hosted arrangement

A host triple that selects a platform takes only the platform's
`[platform.hosted]` part: its toolchain and its layers. With openkal-platform
that is the family's C library and C++ run-time over the system's own openkal
implementation:

```toml
[target.'cfg(any(linux, macos, windows))']
platform = "openkal-platform@0.18.0"
```

On Windows the arrangement is the GNU ABI's row (`--target x86_64-windows-gnu`);
a triple outside `[platform.hosted] targets` is refused, naming the row of the
same system and architecture that it serves.

## The version group

`[platform.versions]` names one version of each package of a family whose
packages pin one another exactly. A graph that holds another version of one of
them is refused before anything compiles:

```
error: openkal-musl 0.22.2 is in this build, and the platform openkal-platform 0.18.0 is a version group that names openkal-musl 0.22.9.
       requested by: own
       The family's packages pin one another exactly, so two versions of one of
       them cannot be linked into one program. Either select the platform release
       of that version ([target.<selector>] platform = "openkal-platform@<version>"), or write
       openkal-musl = "0.22.6" where it is requested.
```

A package taken from a directory or a repository is not bound by the group.

## Writing a platform package

A platform package is a package with a `[platform]` section. openkal-platform,
abridged:

```toml
[package]
namespace = "mcpplibs"
name      = "openkal-platform"
version   = "0.18.0"
mcpp      = ">=2026.10.30.1"

[platform]
targets   = ["x86_64-openkal", "aarch64-openkal"]
toolchain = "llvm@23.1.3"
runner    = { cmd = ["okrun", "{}"], host = "same-arch" }
carrier   = { package = "openkal-run@0.1.3", section = ".oke", main = "carrier/main.cpp" }
plugin    = "build.mcpp"

[platform.layers]
compiler-runtime = "openkal-llvm-runtime@0.16.6"
"c++-abi"        = "openkal-llvm-runtime@0.16.6"
c-abi            = { name = "openkal-musl", version = "0.22.6", default-features = false, features = ["posix"] }

[platform.tools]
okld  = "openkal-tools@0.1.3"
okrun = "openkal-tools@0.1.3"

[platform.build]
cxxflags = ["-fPIC", "-femulated-tls", "-fvisibility=hidden", "-fasynchronous-unwind-tables"]

[platform.build.'cfg(arch = "aarch64")']
cxxflags = ["-ffixed-x18", "-mno-outline-atomics"]

[platform.link]
driver = "lld"
start  = "ok_main"
flags  = ["-shared", "-z", "max-page-size=65536", "-z", "now", "--gc-sections", "--icf=safe", "--eh-frame-hdr"]
inputs = [{ from = ["okld", "note-source"], as = "ok-notes.S" }]
exe    = "{name}.oke"
shared = "lib{name}.okso"
seal   = [["okld", "note", "{}"], ["okld", "check", "{}", "--with", "{needed}"]]
```

- **Programs are named, not located.** `okld` in `inputs` and `seal` is found among
  the programs the graph built for this machine, then in declared payloads, then
  on `PATH`.
- **`{}` is the artifact.** In a seal step `{needed}` expands to the shared images
  the artifact needs; a preceding `--with` is repeated before each.
- **A seal step that fails publishes nothing.** The artifact published by the
  last successful build stays in place.
- **What depends on the graph is code.** `plugin = "build.mcpp"` runs in the
  platform phase (`mcpp::phase()` is `"platform"`) and may state flags, link
  fields and runners (`mcpp::link_driver`, `mcpp::link_input_from`,
  `mcpp::artifact_name`, `mcpp::seal`, `mcpp::run_host`); any other directive is
  refused, except the package formats the platform provides (`pack-format` and
  `artifact` actions, which make e.g. an `okb` from the staged tree). A
  board-support package's build program states the same fields in its build
  phase ([34](34-authoring-a-bsp.md)).
- **The carrier's entry reads what it carries.** mcpp places the carried bytes
  between `mcpp_carried_begin` and `mcpp_carried_end` (the entry's name at
  `mcpp_carried_name`): nothing, when the program is a self-contained tree's
  runner and runs the image its first argument names; the image itself; or a
  stored zip of the images (`PK\x03\x04`, each aligned to 64 KiB) for a
  single file of an image that names others ([SPEC-013](specs/platforms.md)
  §11).

During development a project selects the working tree with
`--platform path:<dir>` or `platform = { path = "<dir>" }`. `mcpp test` in the
platform package, with no `--target`, builds its tests for each of its
`targets` with the package as the platform, runs those of this machine's
architecture and those an execution host is named for (2026.10.29.1+), and
reports the others as built and not run.

## Inspecting a configuration

```sh
mcpp self env --target x86_64-openkal                 # toolchain, platform, tools, runner, and why runs are refused here
mcpp self env --target x86_64-openkal --format json
mcpp self env --host                                  # the host graph, its runner and tools once its build programs were built
mcpp toolchain list                                   # the PLATFORM column
mcpp toolchain install --target x86_64-openkal        # fetch and build what the target needs
```

`--message-format json` carries `platform` and `hostPlatform` in the `plan`
event and in `result.json` ([50](50-machine-output.md)).

## Refusals

| Reason | Cause | Remedy |
|---|---|---|
| `platform-not-a-platform` | the selected package has no `[platform]` | select a platform package, or `none` |
| `platform-target-unserved` | the platform does not serve the triple | build for one of its `targets`, or for the host row it names |
| `platform-tool-absent` | a program of the link or the seal is neither built nor found | name its package under `[target.'cfg(platform = "…")'.tools]` |
| `platform-version-group` | another version of a family package is in the graph | select the platform release of that version, or pin the group's version |
| `platform-phase-directive` | the plugin stated a directive the platform phase does not take | state it from the package's own build program |
| `lld-required-absent` | the platform links with `ld.lld` and the toolchain has none | use the platform's toolchain |

## Current limitations

- A platform states one value per field; a field that differs per architecture
  is written under `[platform.build.'cfg(arch = "…")']` for flags only.
- The host graph's platform is selected per run or per project; its default is
  the host conventions.
- `[host]` accepts `platform` only.
