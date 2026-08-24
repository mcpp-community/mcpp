# 06 — One Source, Several Machines

A program that asks each machine what it is, built for three targets from any
host without being edited.

```bash
mcpp run                                  # this machine
mcpp build --target x86_64-linux          # Linux,   from any host
mcpp build --target aarch64-macos         # macOS,   from any host
mcpp build --target x86_64-windows-gnu    # Windows, from any host
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

## What This Program Does Not Ask

A capability word says how an implementation behaves **within an interface it
provides**. Whether it provides the interface at all is a different question,
and the specification answers it earlier and by other means:

| Time | Mechanism | Question |
|---|---|---|
| dependency resolution | the package declares what it provides | may this program be built against this implementation |
| link | an undefined symbol | was an interface used that the implementation does not provide |
| run | a capability word | how does this implementation behave within an interface it provides |

So this program may ask a filesystem how it compares names, and may not ask a
machine whether it has a filesystem. Building it for `riscv64-none-elf`, whose
backend implements `abort`, `stream`, `memory`, `env` and `time` and nothing
else, produces:

```
ld.lld: error: undefined symbol: kal_fs_props
>>> referenced by fs.cppm:136
>>>               obj/main.o:(kal::fs::properties@openkal.fs())
```

The reference comes from the question rather than from any filesystem call, and
the error is clause 6.1 working: "an interface that an implementation does not
provide is absent as a link-time definition, and a consumer that uses it fails
to link." Defining the word as zero in the backend was tried, and it is the one
thing the clause forbids — the program then proceeds past the point the linker
existed to stop it at.

## What A Bare-Metal Project Writes Instead

A different program, in a different directory, asking only what that machine
provides. Two things must be declared, both properties of the board rather than
defaults:

```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

`sysroot = ""` selects the zero-libc tier. The emulator is named without a path,
and must stay that way: committing an absolute path puts one machine's layout
into a file every other machine reads.

## Bare Metal On x86

Two routes, and neither belongs in this manifest.

A **UEFI application** is PE/COFF entered through the Microsoft x64 calling
convention, so its target is `x86_64-windows-gnu` — the same triple as a Windows
program — distinguished by which implementation of the platform interface the
graph resolved. One manifest cannot resolve two different implementations for one
triple, so it needs its own. See `mcpplibs/openkal-uefi`.

A **kernel** has no firmware services to call. Its target is `x86_64-none-elf`,
the zero-libc tier, entered at its own `_start`, reaching hardware directly.
There is no platform interface beneath it to depend on; what such a program
builds on is `mcpplibs/openarch`, the architecture-mechanism layer.

Both routes are described in
[docs/15 — Cross-Compilation Over openkal](../../docs/15-openkal-cross.md).

## Reference

[docs/15 — Cross-Compilation Over openkal](../../docs/15-openkal-cross.md) for
the model, and [docs/14 — The Target Side](../../docs/14-target-side.md) for the
five layers and the rules that govern them.
