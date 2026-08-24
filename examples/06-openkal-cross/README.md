# 06 — One Source, Several Machines

A program that asks each machine what it is, built for four targets from any
host without being edited.

```bash
mcpp run                                  # this machine
mcpp build --target x86_64-linux          # Linux,   from any host
mcpp build --target aarch64-macos         # macOS,   from any host
mcpp build --target x86_64-windows-gnu    # Windows, from any host
mcpp run   --target riscv64-none-elf      # no operating system at all
```

`src/main.cpp` contains no preprocessor directive and no branch on a target
name. What differs between the builds is which packages the dependency graph
resolved.

## What The Manifest States

```toml
[dependencies]
openkal-llvm-runtime = "0.1.1"

[toolchain]
default = "llvm@22.1.8"
```

One dependency supplies the compiler runtime and the C++ runtime, and depends in
turn on a C library, which depends on whichever implementation of the platform
interface matches the target being built. One line therefore selects three of
the five target-side layers.

The toolchain line names a compiler and nothing else. Where the headers, the C
library, the C++ runtime and the platform implementation come from is not stated
in the manifest at all; the build reports what it resolved:

```
      Target x86_64-windows-gnu → x86_64-w64-windows-gnu
             kernel-abi        openkal        (openkal-windows@0.1.3, graph)
             c-abi             musl           (openkal-musl@0.3.3, graph)
             c++-abi           libc++         (openkal-llvm-runtime@0.1.1, graph)
```

## What The Program Demonstrates

The output is a table of facts about the machine the program landed on. The code
path producing it is identical everywhere; the answers are not.

Measured, same binary source, two targets built on one Linux host:

| Subject | `x86_64-linux` | `x86_64-windows-gnu` |
|---|---|---|
| preopened directories | 2 | 5 |
| case-sensitive paths | yes | no |
| monotonic granularity (ns) | 1 | 100 |

A portable program written against a conventional stack is a program that
compiles under several sets of `#if`s. A program written against a named
interface is one source that queries what it landed on. The queries in
`src/main.cpp` are chosen so that each exercises a different layer of the target
side, and so that a build which silently reached the host's own libraries
instead of the resolved ones would answer differently.

The unwinding check is the strictest of them. Unwinding is the one part of a C++
runtime that links successfully whether or not it works: a program whose
unwinder is absent still builds, and fails only when something is thrown.
Running a destructor during the unwind separates the two.

## Bare Metal

`riscv64-none-elf` has no operating system beneath it. The `[target]` section
declares the zero-libc tier and names the emulator, because which machine model
and which firmware mode to use are board facts rather than defaults:

```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

The emulator is named without a path, and must stay that way: committing an
absolute path puts one machine's layout into a file every other machine reads.

⚠️ This target requires `openkal-opensbi` at a version that defines every
capability word. Earlier versions omit the ones that read zero, and a program
that merely asks whether a filesystem exists then fails to link:

```
ld.lld: error: undefined symbol: kal_fs_props
>>> referenced by fs.cppm:136
>>>               obj/main.o:(kal::fs::properties@openkal.fs())
```

The reference comes from the question rather than from any filesystem call. See
`mcpplibs/openkal-opensbi#2`.

## Bare Metal On x86

An x86_64 machine with no operating system is reached through UEFI, and a UEFI
application is PE/COFF entered through the Microsoft x64 calling convention.
Its target is therefore `x86_64-windows-gnu` — the same triple as a Windows
program — distinguished by which implementation of the platform interface the
graph resolved, and by three link flags that select the EFI subsystem.

That combination needs its own manifest, since one manifest cannot resolve two
different platform implementations for one triple. See `mcpplibs/openkal-uefi`.

## Reference

[docs/15 — Cross-Compilation Over openkal](../../docs/15-openkal-cross.md) for
the model, and [docs/14 — The Target Side](../../docs/14-target-side.md) for the
five layers and the rules that govern them.
