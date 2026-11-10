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
targets is. An image is a release form, and a native row is a build target with
the same guarantees; neither ranks above the other ([24](24-openkal-cross.md),
"Two Roles, No Ranking").

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
             the platform's (openkal-platform 0.24.0), replacing your gcc@16.1.0 — override with `[target.x86_64-openkal] toolchain`
      Target x86_64-openkal → x86_64-unknown-none-elf
             compiler-runtime  compiler-rt    (openkal-llvm-runtime@0.19.0, platform)
             c-abi             musl           (openkal-musl@0.28.0, platform)
             c++-abi           libc++         (openkal-llvm-runtime@0.19.0, platform)
    Platform openkal-platform 0.24.0 (built-in)
             tools             okld, okprof, okrun (openkal-tools@0.5.0, platform)
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
| the compile | `[platform.build]`, `[platform.build.'cfg(…)']` | flags for every unit of the target, the std module included (2026.11.3.1) |
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
| the command line | `--platform openkal-platform@0.24.1`, a series `--platform openkal-platform@^0.24`, `--platform path:../openkal-platform`, `--platform none` |
| the environment | `MCPP_TARGET_X86_64_OPENKAL_PLATFORM=…` |
| the project | `[target.<selector>] platform = "openkal-platform@0.24.0"`, or `{ path = "…" }` |
| this machine | the same key in `~/.mcpp/config.toml` |
| the target table | the platform column (`mcpp toolchain list`) |

`platform` is not accepted under a `cfg(platform = …)` selector. `none` builds
with the engine's conventions for the target and nothing else.

## The host graph's platform

Build programs and the tools the build runs are compiled for this machine by
its own conventions. `[host] platform` selects a platform for them instead:

```toml
[host]
platform = "openkal-platform@0.24.0"
```

```
        Host platform openkal-platform 0.24.0 (mcpp.toml:2): build programs are x86_64-openkal images run by okrun
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
platform = "openkal-platform@0.24.0"
```

On Windows the arrangement is the GNU ABI's row (`--target x86_64-windows-gnu`);
a triple outside `[platform.hosted] targets` is refused, naming the row of the
same system and architecture that it serves.

A C library of the graph (`mcpp:c-abi`) leads a Mach-O image's link
(2026.11.9.1+). The loader of that format runs an image's initialisers before
its entry, in link order, and the C library is brought up by an initialiser of
its own; with its objects first, it is up before any global constructor of the
program runs, as a C runtime's start objects precede a program on ELF.

## The version group

`[platform.versions]` names one version of each package of a family. From mcpp
2026.11.7.1 the group chooses: the platform is the first package the graph
resolves, and each later request of a package the group lists, by version,
resolves to the group's version when its constraint admits it, though the index
holds a newer patch. openkal's packages name one another by compatible ranges
(`openkal-musl = "^0.28.0"`), so the group's choice is what a build links. A
request the group's version does not satisfy is refused before anything
compiles:

```
error: openkal-musl is requested as '^0.27.0' by app, and the platform openkal-platform 0.24.1 is a version group
       that names openkal-musl 0.28.1.
       The group chooses one version of each package of the family for a
       build. Either select the platform release of the version you need
       ([target.<selector>] platform = "openkal-platform@<version>"), or admit 0.28.1 where
       it is requested (openkal-musl = "^0.28.1").
```

A package built for this machine only (a tool's) and a package taken from a
directory or a repository are not bound by the group. An mcpp before
2026.11.7.1 checks the group after resolution instead; a family released as one
index change resolves the same either way.

## The built-in platform is one release

The target table selects one openkal-platform release for the two ok-image
targets, the one this mcpp was qualified with: 0.25.0 for mcpp 2026.11.10.1,
0.24.4 for 2026.11.8.1 and 2026.11.9.1, 0.24.3 for 2026.11.7.2 (2026.11.7.1 selected the series `^0.24`). A project that states no platform
builds the same versions at any time, with or without `mcpp.lock`; when the lock
records another release, the build takes the default and says which release the
lock recorded. A newer platform needs no new mcpp: a project selects it in its
manifest,

```toml
[target.'cfg(os = "openkal")']
platform = "openkal-platform@0.26.0"     # a release, or a series: "openkal-platform@^0.26"
```

and a build that fails while the default is in use and the synced index holds a
newer release ends with a `tip:` naming both and this line.

A series takes its newest release whose `[package] mcpp` floor this mcpp meets,
naming each release it skips for a newer engine, and `mcpp.lock` records it; the
next build takes the recorded release while the series admits it. `mcpp update`
(or `mcpp update openkal-platform`) drops the record, and the build after it
takes the newest release again.

## Writing a platform package

A platform package is a package with a `[platform]` section. openkal-platform,
abridged:

```toml
[package]
namespace = "mcpplibs"
name      = "openkal-platform"
version   = "0.24.0"
mcpp      = ">=2026.11.3.1"

[platform]
targets   = ["x86_64-openkal", "aarch64-openkal"]
toolchain = "llvm@23.1.3"
runner    = { cmd = ["okrun", "{}"], host = "same-arch", hosts = ["linux-x86_64", "linux-aarch64", "macos-aarch64", "windows-x86_64"] }
carrier   = { package = "openkal-run@0.5.0", section = ".oke", main = "carrier/main.cpp" }
plugin    = "build.mcpp"

[platform.layers]
compiler-runtime = "openkal-llvm-runtime@0.19.0"
"c++-abi"        = "openkal-llvm-runtime@0.19.0"
c-abi            = { name = "openkal-musl", version = "0.28.0", default-features = false, features = ["posix", "malloc-mimalloc"] }

[platform.tools]
okld  = "openkal-tools@0.5.0"
okprof = "openkal-tools@0.5.0"
okrun = "openkal-tools@0.5.0"

[platform.build]
cxxflags = ["-fPIC", "-femulated-tls", "-fvisibility=hidden", "-fasynchronous-unwind-tables", "-Xclang", "-fstack-clash-protection", "-ffunction-sections", "-fdata-sections", "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer", "-fstack-protector-strong", "-mstack-protector-guard=global", "-ftrivial-auto-var-init=zero", "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST"]

[platform.build.'cfg(arch = "aarch64")']
cxxflags = ["-ffixed-x18", "-mno-outline-atomics", "-mbranch-protection=pac-ret+bti"]

[platform.link]
driver = "lld"
start  = "ok_main"
flags  = ["-shared", "-z", "max-page-size=65536", "-z", "pack-relative-relocs", "-z", "now", "-z", "noexecstack", "--hash-style=both", "--gc-sections", "--icf=safe", "--eh-frame-hdr", "--build-id=sha1", "--emit-relocs", "-mllvm", "-emulated-tls"]
inputs = [{ from = ["okld", "note-source"], as = "ok-notes.S" }]
exe    = "{name}.oke"
shared = "lib{name}.okso"
seal   = [["okld", "note", "{}"], ["okld", "place", "{}"], ["okld", "check", "{}", "--with", "{needed}"]]
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

## Defining targets and providers (2026.11.9.1+)

A platform may serve a system no built-in row names. `[[platform.define]]`
states the row, `[[platform.provider]]` the provider that serves it:

```toml
[[platform.define]]
triple        = "x86_64-haiku"
clang-triple  = "x86_64-unknown-haiku"
object-format = "elf"
family        = "unix"
execution     = "haiku"            # the system its programs run on

[[platform.provider]]
serves   = ["x86_64-haiku"]        # patterns: `*` is any one segment
priority = 100
hosted   = true                    # the C environment has `main`
```

With the platform selected (`[target.x86_64-haiku] platform = {...}` or
`--platform`), `--target x86_64-haiku` builds into `target/x86_64-haiku/`,
`cfg(os = "haiku")` holds, and a program whose system is not this machine's
is built, not run, until a runner states how it runs (docs/41). The row's
machine description is the platform's: a target whose machine differs from a
built-in row is another row. `[platform]` and `[platform.hosted]` remain the two
shorthands of a provider.

**A defined row is read as a built-in one (2026.11.10.1+).** Its `march` and
`link-emulation` reach the compile and an `ld.lld` link (`mcpp why flags` marks them
`row`); nothing the toolchain payload built for its own host — its libc++ headers,
its runtime directories, the host's `-L` and RPATH — reaches the row; and the host
loader's closure and symbol checks apply only when the row's execution system is
this machine's. The same holds for a built-in row no built-in provider serves.

**A system the built-in rows name is served, not defined (2026.11.10.1+).**
FreeBSD's rows are built-in (`x86_64-freebsd`, `aarch64-freebsd`); a platform that
serves them states a provider and a base for each, and defining them again is an error.
A provider's `sysroot` (a package, or `{ path = "<dir>" }` relative to the platform's
manifest) is that identity's base, ahead of the platform's own `sysroot`:

```toml
[platform]
toolchain = "llvm@23.1.3"

[[platform.provider]]
serves  = ["x86_64-freebsd"]
sysroot = "xim:x86_64-freebsd-sysroot@14.5"     # a base package ([22](22-target-side.md))
hosted  = true

[[platform.provider]]
serves  = ["aarch64-freebsd"]
sysroot = "xim:aarch64-freebsd-sysroot@14.5"
hosted  = true

[platform.build]
ldflags = ["-pthread"]

[platform.link]
driver = "compiler"
```

**`[platform.link] flags` are words for ld.lld (2026.11.10.1+).** A platform whose
`driver` is `"compiler"` gives them to no program, and the build says so, naming
`[platform.build] ldflags`, whose words reach the compiler's link.

`mcpp new <name> --template platform` writes a platform package defining
`<arch>-<name>`. Reading a platform's manifest checks its statement against the
rows: a definition of a built-in row, or a row defined twice, is an error
wherever the platform is read; a provider that serves no row, and two providers
that serve one identity at one priority, are warnings of the platform's own
build. `mcpp why platform` lists the rows the platform defines and the
identities it serves, each with its provider and priority: in the platform's
directory its own, in a project the one its target selects (`--target`;
`--format json`).

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
