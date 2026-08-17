# 12 - Distributing a Prebuilt Library

**English** | [简体中文](zh/12-binary-distribution.md)

> Ship a library as **interface + prebuilt binaries** instead of as source.
> It applies to closed-source distribution, offline environments, and builds whose
> artifacts a build farm has already produced.
>
> Related: [02 - Packaging & Release](02-pack-and-release.md) covers bundling an
> *application*. [10 - Publishing a Library](10-publishing-a-library.md) is the
> source route.

## Overview

`mcpp pack <target>` builds a library target and writes a directory that is an
**ordinary mcpp package** — a normal `mcpp.toml`, the interface a consumer must
compile, and the binaries it then links. Consumers use it exactly like any
other dependency. There is no new manifest section, no new archive format, and
no new resolution path.

```bash
mcpp pack mathkit                              # a static library package
mcpp pack mathkit-shared                       # a dynamic one (ELF, Mach-O, PE/MinGW)
mcpp pack mathkit --target x86_64-linux-gnu \
                  --target aarch64-linux-gnu   # one package, two legs
```

## What determines the package contents

`[targets.<name>].kind`, and nothing else:

| `kind` | `mcpp pack <name>` produces | `--mode` |
|---|---|---|
| `bin` | an application bundle (see [02](02-pack-and-release.md)) | the four depths |
| `lib` | a **static library package** | — |
| `shared` | a **dynamic library package** | — |

There is no `--lib` flag and no `--artifact static\|shared`. `kind` is already
where mcpp records what an artifact is; a flag would be a second place to say
it, and two places can disagree. A project that publishes both forms declares
both targets — which it must do for `mcpp build` to produce both anyway:

```toml
[targets.mathkit]
kind = "lib"

[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
```

Run `mcpp pack` with no name and mcpp picks the only packable target, or tells
the candidates it found.

## The two interface modes

A package can carry both, and a consumer may use either or both.

```
mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23/
├── mcpp.toml
├── include/          ← TEXT interface: #include, never compiled
├── interface/        ← MODULE interface: the consumer compiles it
└── lib/<triple>/     ← the artifacts
```

| | `include/` | `interface/` |
|---|---|---|
| whose input is it | the preprocessor's | the **compiler's** |
| does the consumer compile it | no | **yes**, to get a BMI |
| what it constrains | the libc ABI | compiler, C++ stdlib, C++ level |
| trimmable | **no** — see below | **no**, it is computed |

`lib/` is keyed by **triple**, not by OS. MinGW and MSVC are both Windows and
produce `libfoo.a` and `foo.lib` respectively.

A **shared** package carries the library under *both* of its names: a consumer
links `lib<target>.so` and the loader then asks for the `SONAME`, and those are
different filenames. Shipping only the built file links cleanly and then fails
to start.

### Why neither set may be trimmed

A **source** distribution of the same package puts every one of its
`include_dirs` on its consumers' include path. If a binary package shipped a
subset, the same library would have a different public surface depending on how
it was delivered. "Which headers are public" is already answered by the layout:
`include/` is public, `src/` is not. A private header under `include/` is a
project-layout mistake, not a packaging option.

## Interface units included in the package

The **module closure of the lib root** — `src/<package-tail>.cppm` by
convention, or `[lib].path`. Whatever that unit's purview imports, transitively,
is published; everything else is not.

```
src/mathkit.cppm    export module mathkit;  export import :api;   → published
src/api.cppm        export module mathkit:api;                    → published
src/secret.cppm     module mathkit:secret;         ← implementation partition
src/impl.cpp        module mathkit;                                → withheld
```

`mcpp pack` prints both lists:

```
   Interface mathkit.cppm, api.cppm
    Withheld capi.c, impl.cpp, secret.cppm
```

**The second row is the one that matters for closed-source distribution.**

> **`.m.o` is not the rule.** An implementation partition produces a BMI and an
> object exactly like an interface unit does. Selecting sources by "does it
> produce a BMI" would publish `secret.cppm`.

If the published interface *does* import an implementation partition, a
consumer cannot compile it without that source — so `mcpp pack` stops:

```
error: the published interface imports mathkit:secret , which no unit in this
       build provides.
```

Restructure so the interface does not reach it, or make it an `export module`
partition and accept that its source is published.

## The compatibility tag

Every artifact records the toolchain it was built for:

```
x86_64-linux-gnu-gcc16-libstdcxx16-c++23      # a C++ module interface
x86_64-linux-gnu                              # an extern "C" interface only
```

`<arch>-<os>-<env>` then, when the interface is C++, `<compiler><major>`,
`<stdlib><major>`, `c++<level>`.

**A shorter tag is a real statement, not a missing one.** A library whose whole
interface is `extern "C"` constrains the libc ABI and not the C++ one, so it
publishes a triple and stops — and links into any compiler. Unnamed dimensions
are don't-care, so one tag per triple instead of one per triple per compiler.
Nothing to configure: the shape is the statement.

The `c++` level is compared as a **floor**, not for equality: building at a
higher level is fine, lower is not.

## Consumer-side build checks

Two things, both of which fail silently without a check:

**The interface still matches its binaries.**

```
error: acme.mathkit@0.1.0: 'interface' does not match what was packaged.
  recorded fnv1a:25b2cf2a79d71c40
  found    fnv1a:fe404d5be85118ff
```

This exists because the alternative was measured. Swap two `int` members of a
struct in a shipped interface — the Itanium ABI does not mangle field order —
and the consumer compiles, links, runs, and prints transposed data, with no
diagnostic from any tool. A digest cannot stop a publisher from shipping a
mismatched pair (only producing both in one command does that), but it does
catch the pair coming apart afterwards.

**The binaries were built for this toolchain.**

```
error: acme.mathkit@0.1.0: no prebuilt artifact matches this toolchain.
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23
  published tags :
                   x86_64-linux-gnu-gcc15-libstdcxx15-c++23
  closest is x86_64-linux-gnu-gcc15-libstdcxx15-c++23, and it differs on:
    compiler  needs gcc15, this build has gcc16
    stdlib    needs libstdcxx15, this build has libstdcxx16
```

The tags it *does* have are part of the message: "not found" would send the
reader looking for a package that is already on disk.

## Consuming a package

Three spellings, one code path:

```toml
# a directory (copied directly to a colleague)
mathkit = { path = "vendor/mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23" }

# a private git repo
mathkit = { git = "ssh://git@internal/mathkit-dist.git", tag = "v0.1.0" }

# an index entry — identical in shape to a source package's
mathkit = "0.1.0"
```

Nothing about the consumer's manifest says "this one is prebuilt".

### Building inside a package is refused

```
error: … is a distribution package produced by `mcpp pack`, not a source tree.
```

Its `interface/` holds declarations whose definitions are in the archive beside
them. Building there compiles the declarations, produces a near-empty library
and reports success.

## One package, several targets

`--target` is repeatable. The generated manifest gets one conditional block per
leg, and the consumer's build picks its own:

```toml
[target.'cfg(all(arch = "x86_64", os = "linux", env = "gnu"))'.build]
ldflags = ["-Llib/x86_64-linux-gnu", "-lmathkit"]

[target.'cfg(all(arch = "x86_64", os = "linux", env = "musl"))'.build]
ldflags = ["-Llib/x86_64-linux-musl", "-lmathkit"]
```

Because selection happens in the **consumer's** build, where the resolved
target is known, a multi-target package cross-compiles correctly with no index-side or
installer-side support at all.

> The blocks are `cfg(...)` and never a bare `[target.'<triple>']` key. Before
> mcpp 2026.8.18.1 the bare form was inert without an explicit `--target`, so a
> package using it would work in CI and silently drop its flags on a
> developer's machine. mcpp generates the spelling that means the same thing on
> every client.

## Dependencies

A static archive does **not** carry its dependencies' code, so the package
records them and the consumer resolves them:

```toml
[dependencies]
"compat.zlib" = "1.3.2"
```

`path` and `git` dependencies are dropped: they address the publisher's disk,
and republishing one hands the consumer an address that means something else.
A library that depends on one must either publish that dependency too, or vendor it
before packing.

## Behaviour of older mcpp releases

**It builds against them.** Every key in the generated manifest already
existed, so an older client reads the package and links it. What it does not do
is run the two checks above — it has no way to know that `provenance =
"mcpp-pack …"` means anything.

That is a degradation, not a break, and it is the right direction. But it means
**the gate protects new clients only**, which belongs in the release notes of any
package published to a mixed-version audience.

## Current limitations

| | status |
|---|---|
| `kind = "lib"` (static) | ✅ every target, tested on all three |
| `kind = "shared"` on Linux/ELF | ✅ — the package carries both the link name and the SONAME |
| `kind = "shared"` on PE / MinGW (`*-windows-gnu`) | ✅ — the package carries the `.dll` **and** its import library |
| `kind = "shared"` on Mach-O (`*-macos`) | ✅ — install name is `@rpath/<file>`, so the `.dylib` relocates |
| `kind = "shared"` on PE / MSVC (`*-windows-msvc`) | ❌ refused — see below |
| `kind = "shared"` on `*-musl` | ❌ a musl target links statically |
| shipping prebuilt BMIs | ❌ not attempted; BMIs are compiler-build-exact |
| bundling dependencies into the package | ❌ declare them instead (above) |
| consuming a package with **native `cl.exe`** | ❌ see below |

### Why MSVC refuses `kind = "shared"`

Not the linker — `link /DLL /IMPLIB:` works. **Symbol export.** MSVC exports
nothing from a DLL unless the source says `__declspec(dllexport)` or a `.def`
file lists the symbols, so the import library comes out empty and every consumer
fails with unresolved externals naming symbols that are plainly in the object
files. mcpp refuses rather than produce a diagnostic that points nowhere near its
cause:

```
target 'mathkit': kind = "shared" is not supported for the MSVC ABI (x86_64-windows-msvc).
  MSVC exports nothing from a DLL unless the source says `__declspec(dllexport)`
  ...
  Use kind = "lib" for this target, or build it for *-windows-gnu (MinGW),
  where the linker auto-exports.
```

MinGW's linker auto-exports, which is why `*-windows-gnu` is supported and
`*-windows-msvc` is not. Closing this needs a generated `.def` — a symbol scan
over the objects — which is a build-graph node, not a flag.

### A package's link flags are GNU-spelled

The generated manifest selects each leg with

```toml
[target.'cfg(all(arch = "x86_64", os = "windows", env = "msvc"))'.build]
ldflags = ["-Llib/x86_64-windows-msvc", "-lmathkit"]
```

Every driver mcpp uses accepts that — including clang on the MSVC ABI, which is
Windows' default here. **Native `cl.exe` does not**: it rejects `-L`. So a
consumer that pins `[toolchain] windows = "msvc@system"` cannot link a packaged
library today.

Naming the file by path instead (`lib/<triple>/mathkit.lib`) is the spelling
every driver takes, and it does not work either: ninja runs link commands with
cwd = the output directory, and only the include-family prefixes (`-I`, `-L`, …)
are absolutized against the package root, so a prefix-less token is looked for
in the wrong place — `ld: cannot find lib/x86_64-windows-gnu/libmathkit.a`. A
manifest cannot carry an absolute path and stay relocatable.

The dialect-neutral channel does exist — `[runtime] link_library_dirs` and
`libraries`, which mcpp renders as `/LIBPATH:` + `name.lib` or `-L` + `-lname`
depending on the target — but only at the top level, and a package needs it
**per leg**. Measured: a previous mcpp reads `[target.'cfg(…)'.runtime]` without
complaint and silently ignores it. That is the wrong kind of tolerance for this
purpose — moving a leg's link flags there would leave every older client with no
link flags at all, so closing this properly means either carrying both spellings
(and linking the library twice on new clients) or giving these packages a version
floor.

### Implementation partitions

`mcpp pack` treats an implementation partition (`module M:part;`, no `export`)
as private: its source stays behind, its object ships inside the archive.

If the published interface *imports* one, the consumer cannot build the BMI
without that source, so it IS published — and `mcpp pack` says so:

```
warning: secret.cppm is an implementation partition, and the published interface
         reaches it — so its SOURCE is being published.
```

> Until mcpp 2026.8.18.1 the scanner recorded `module M:part;` as *requiring*
> `M:part` and providing nothing, so a file required its own name and the graph
> held no edge from the unit importing a partition to the unit defining it.
> Build order was unconstrained: GCC and macOS clang recovered through their own
> dependency scan, Windows clang failed with `failed to read compiled module`.
> Implementation partitions were unusable on Windows for this reason.

### Partitions whose kind cannot be determined

`[scan_overrides."<glob>"]` says which modules a file provides and has nowhere to
say whether the declaration carries `export`; a P1689 scanner may omit
`is-interface`. Either way the source is published — the consumer cannot build the
BMI without it — and `mcpp pack` reports which of the two situations applies:

```
warning: secret.cppm provides a module PARTITION and mcpp cannot tell which kind:
         the unit is declared in `[scan_overrides]`, which has nowhere to say
         whether the declaration carries `export`, …
```

> Until 2026.8.18.1 that arrived as "it is an interface" — the answer that
> produces **no** warning — so an implementation partition declared that way was
> published in silence. Publishing too few sources fails the consumer's compile
> and names the module; publishing too many ships private source and nothing
> fails at all. Unknown has to be loud.

## Verification scope

The e2e suite gates each test on host capabilities, so "the suite is green" and
"this ran" are different statements. What runs where:

| claim | linux | macOS | windows |
|---|---|---|---|
| layout, both interface modes, closure, the two gates, workspace root, named target, `sources = []`, bare-triple predicate | ✅ | ✅ | ✅ |
| multi-target package, two legs one artifact name (`gnu` + `musl`) | ✅ | *impossible* | — |
| multi-target package, two legs **two** artifact names (`msvc` + `mingw`) | — | *impossible* | ✅ |
| multi-target package crossing an OS boundary (a PE leg) | ✅ | — | — |
| `lib.exe /REMOVE:` really removing | — | — | ✅ |
| PE shared library: build, pack, link, run | ✅ (wine) | — | — |
| Mach-O shared library relocating out of its build tree | — | ✅ | — |
| MSVC refusing `kind = "shared"` for the export reason | — | — | ✅ |
| a released mcpp consuming a package this one produced | local only | local only | local only |

*impossible* is not a gap: a macOS host can serve exactly one target
(`host_can_serve`, `registry.cppm`), so a package with two legs cannot be produced
there at all.

The last row is honest about a real hole: each CI job bootstraps from a released
mcpp, but that entry is an xvm **shim**, and under the e2e suite's environment it
answers "not installed". So the static half of the old-client check (the generated
manifest uses no section a previous mcpp cannot read) runs everywhere, and the
real half — build against the package with the previous release — has been run by
hand, not by CI.
