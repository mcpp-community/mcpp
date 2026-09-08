# 00 — What mcpp Is

## Background: the state of C++ project tooling

A C++ project needs four things in place at the same time: a build description,
a set of dependencies, a compiler new enough for the code, and an environment in
which the result runs. No single tool in C++ owns all four. Four unrelated
classes of tool each carry one, and aligning them is the project's own work.

CMake is the de facto standard for the first of the four. A de facto standard is
a statement about adoption, not about the experience of use. CMake does not
resolve dependencies, does not install a compiler and does not describe the
runtime environment, and those three are what a new contributor meets on the
first day. Supplying them means adding a package manager, the distribution's
packages, and a paragraph of instructions in a README: three further models,
with the alignment between them left to the project.

The environment is the layer that fails most often, because it is the only one
nothing checks. A build description reports an error at configure time and a
package manager fails to resolve, but a library present on the machine that is
not the library the code was built against produces no signal until the link,
and sometimes not until the program runs.

Modules add one more constraint to that structure. C++20 modules change what a
translation unit costs: an interface is declared once and imported instead of
being re-parsed out of headers by every file that needs it, and a consumer sees
what the author exported rather than everything a header happened to include.
The price is that a language feature is now implemented by the build system. The
build scans sources for `import`, orders the compiles accordingly, caches the
compiled interfaces and invalidates them correctly, on a compiler new enough to
have the feature at all. `import std` adds a further requirement: the standard
library's own module is built before anything else can use it.

A project that adopts modules therefore needs all four layers rather than three.
The gap is concrete. On the machine this chapter was written on:

```console
$ g++ --version
g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
```

That compiler cannot compile `import std`. Nothing about the project is wrong;
the machine is not the machine the project requires.

## The cost of that state

**For a person the cost is a setup**, repeated once per machine and once per new
contributor: install a newer compiler, determine which build flags enable
modules, obtain the dependencies, and then determine which of the three is at
fault when the link fails. The cost does not fall as a project matures. It
repeats per person and per machine.

**For an agent the cost is context**, spent in three places before a line of
code is written: reading the build description to determine what it does,
reconstructing the environment it assumes, and following a header through its
transitive includes to determine what is declared.

Modules remove the third, because an interface is explicit and `import` states
what is used. mcpp removes the other two, and reduces the first to one command.

## The composition of mcpp

```
mcpp = build system
     + build plugins
     + package manager
     + toolchain management
     + the environment and runtime (xlings)
```

Most C++ projects assemble those five parts from separate tools, and the setup
cost described above is the cost of that assembly: the build file assumes a
compiler the machine does not have, the package manager assumes a build file it
did not write, and the environment is a paragraph in a README.

mcpp is one program, and the five parts share one model.

For a reader who already uses tools for these jobs:

| component | form in mcpp | comparable tools |
|---|---|---|
| build system | `mcpp.toml`, the module graph, the ninja backend | CMake, Meson |
| build plugins | `build.mcpp`, rule packages | `build.zig`, xmake rules |
| package manager | `[dependencies]`, `mcpp.lock`, the index | Conan, vcpkg |
| toolchain management | the compiler as an installed, pinned payload | Zig's bundled toolchain, rustup, manual GCC / LLVM / MSVC installation |
| environment and runtime | `[xlings]`, payloads, the runtime search path | Nix, conda |

The closest single-tool analogues are Cargo and Zig, each for a different half
of the same idea. Cargo is one program that is the build, the package manager,
the lock file and the test runner, so a Rust project is cloned and built with no
preliminary step. Zig ships its toolchain with the tool and cross-compiles by
default, so the compiler is not something the machine must already have.

mcpp is that shape for C++, with one part neither of them has: the environment
layer, through which a project declares the non-compiler tools its build
requires.

**The table places the parts; it does not claim equivalence.** Each tool listed
does more in its own area than mcpp does, and a project that needs that depth
uses it.

## The guarantee

> **Clone any mcpp project and `mcpp build` works** — without installing a
> compiler, configuring an environment, or locating dependencies.

Two boundaries are stated here so that the claim can be relied on: a project
that targets a device still downloads that device's toolkit on the first build,
and a target this machine cannot serve is refused by name rather than built
incorrectly.

## A minimal example

On the same machine, whose only C++ compiler is the GCC 13 above.

```console
$ mcpp new hello
Created bin package 'hello' at /tmp/zero-demo/hello
Next: cd hello && mcpp build && mcpp run  (or `mcpp test`)
```

Four files, and the manifest is five lines:

```toml
[package]
name        = "hello"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"
```

**No compiler, no language standard and no dependency is declared.** That is the
whole of what a reader has to understand before changing this project. The
source uses the feature the machine's compiler does not have:

```cpp
import std;

int main() {
    std::println("Hello from hello!");
}
```

```console
$ mcpp run
    Inferred target hello (bin from src/main.cpp)
   Compiling hello v0.1.0 (.)
    Finished dev [unoptimized + debuginfo] in 0.64s
     Running `target/x86_64-linux-gnu/0946988e9e4b52ba/bin/hello`

Hello from hello!
Built with import std + std::println on modular C++23.
```

1.25 seconds of wall clock, first run included. The compiler that performed it:

```console
$ mcpp self env
default toolchain   = gcc@16.1.0
```

mcpp installed GCC 16 and used it. Nothing on the host was modified, and a
colleague — or an agent — cloning this project receives the same compiler rather
than the one their machine happens to ship.

**A dependency is one line and requires no further step:**

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` resolves it, fetches it, builds it and links it.

## Scope

mcpp is built around **C++20/23 modules and recent language features**, and the
ecosystem it maintains follows from that rather than from a general ambition:

| | |
|---|---|
| modular C++ | `import std` with no configuration, module scanning, a cross-project BMI cache |
| embedded and bare metal | freestanding targets, board-support packages, one command from source to a running image |
| heterogeneous computing and GPUs | CUDA, HIP, SYCL, Vulkan/SPIR-V and Ascend C, each a rule package rather than an engine feature |
| graphics | shaders compiled as part of the build and reached as modules |
| kernel and low-level work | zero-libc tiers, an explicit link model, no hidden host dependency |

A project that requires none of these still receives the guarantee above; a
project that requires one of them does not leave the tool to obtain it.

## Reading path

| | |
|---|---|
| run a first program | [01 — Getting Started](01-getting-started.md) |
| determine whether mcpp fits the work at hand | [02 — Scenarios](02-scenarios.md) |
| read a project of the same shape | [03 — Examples](03-examples.md) |

Everything after this chapter is reference: what a manifest may state, how
dependencies resolve, how a target is named. This chapter states no field and no
flag by design.
