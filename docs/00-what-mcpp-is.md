# 00 — What mcpp Is

**Reader:** anyone, before anything else.

**The question this chapter answers:** what is mcpp, what does it do for a
project, and what does it cost to try.

**Not here:** how any of it works internally, and every field and flag. This
chapter ends with a session that has been run; the chapters after it are the
reference.

## Five things in one program

```
mcpp = build system
     + build plugins
     + package manager
     + toolchain management
     + the environment and runtime (xlings)
```

Most C++ projects assemble those five from separate tools, and the seams between
them are where a new contributor loses an afternoon: the build file assumes a
compiler the machine does not have, the package manager assumes a build file it
did not write, and the environment is a paragraph in a README.

mcpp is one program, so there are no seams to assemble.

For a reader who already has tools for these jobs, the parts land roughly here:

| the part | in mcpp | roughly the job of |
|---|---|---|
| build system | `mcpp.toml`, the module graph, the ninja backend | CMake, Meson |
| build plugins | `build.mcpp`, rule packages | `build.zig`, xmake rules |
| package manager | `[dependencies]`, `mcpp.lock`, the index | Conan, vcpkg |
| toolchain management | the compiler as an installed, pinned payload | Zig's bundled toolchain; rustup's role in Rust; installing GCC / LLVM / MSVC by hand |
| environment and runtime | `[xlings]`, payloads, the runtime search path | Nix, conda |

**The closest single-tool analogues are Cargo and Zig**, and for different
halves of the same idea. Cargo is one program that is the build, the packages,
the lock file and the test runner, so a Rust project is cloned and built without
a preliminary step — that is the guarantee below, in another language. Zig ships
its toolchain with the tool and cross-compiles by default, so the compiler is
not something the machine has to already have — that is the toolchain row.

mcpp is that shape for C++, with one part neither of them has: the environment
layer, which is why a project can also declare the *non-compiler* tools its
build needs.

**The table places the parts; it does not claim equivalence.** Each of those
tools does more in its own area than mcpp does, and a project that needs that
depth should use it. What the row is saying is which familiar job the part
corresponds to, so the rest of this documentation has somewhere to attach.

## The guarantee

> **Clone any mcpp project and `mcpp build` works** — without installing a
> compiler, configuring an environment, or hunting down dependencies.

That is the whole claim, and everything below is it being demonstrated rather
than repeated.

Two boundaries, stated here so the claim can be trusted: a project that targets
a device still downloads that device's toolkit the first time, and a target this
machine cannot serve is refused by name rather than built wrongly.

## A first session, end to end

Run on a machine whose only C++ compiler is GCC 13, which cannot compile
`import std`.

```console
$ mcpp new hello
Created bin package 'hello' at /tmp/zero-demo/hello
Next: cd hello && mcpp build && mcpp run  (or `mcpp test`)
```

Four files, and the manifest is five lines:

```
hello/
├── mcpp.toml
├── src/main.cpp
├── tests/test_smoke.cpp
└── .gitignore
```

```toml
[package]
name        = "hello"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"
```

**No compiler, no language standard and no dependency is declared**, and the
source uses a feature the machine's own compiler does not have:

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

1.25 seconds of wall clock, first run included.

The compiler that did it is not the one on the machine:

```console
$ g++ --version
g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0

$ mcpp self env
default toolchain   = gcc@16.1.0
```

mcpp installed GCC 16 and used it. Nothing on the host was changed, and a
colleague who clones this project gets the same compiler rather than the one
their distribution happens to ship.

**A dependency is one line and needs no other step:**

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` resolves it, fetches it, builds it and links it.

## What mcpp is for

mcpp is built around **C++20/23 modules and the newest language features**, and
the ecosystem it maintains follows from that rather than from a general
ambition:

| | |
|---|---|
| modular C++ | `import std` with no configuration, module scanning, a cross-project BMI cache |
| embedded and bare metal | freestanding targets, board-support packages, one command from source to a running image |
| heterogeneous computing and GPUs | CUDA, HIP, SYCL, Vulkan/SPIR-V and Ascend C, each a rule package rather than an engine feature |
| graphics | shaders compiled as part of the build and reached as modules |
| kernel and low-level work | zero-libc tiers, an explicit link model, no hidden host dependency |

A project that wants none of those still gets the guarantee above; a project
that wants one of them does not leave the tool to get it.

## Where to go next

| | |
|---|---|
| put a program on the screen | [01 — Getting Started](01-getting-started.md) |
| decide whether mcpp fits the work at hand | [02 — Scenarios](02-scenarios.md) |
| read a project of the same shape | [03 — Examples](03-examples.md) |
