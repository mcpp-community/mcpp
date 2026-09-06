# Cross-Compilation Over openkal

Conventional cross-compilation is served by a payload. A toolchain is built for
one target, its driver has exactly one answer, and reaching a second target
means obtaining a second toolchain. The number of payloads a distribution must
publish is therefore the number of host-target pairs it supports.

openkal changes what is being crossed. The target side — the platform interface,
the C library, the compiler runtime and the C++ runtime — becomes a set of
packages resolved from the dependency graph and compiled from source by whichever
compiler is running. What remains for the compiler is code generation, and one
Clang binary emits every object format it was built with.

This document states the model, what a project writes, what the ecosystem
supplies, and the limits that have been measured.

## The Claim

An ecosystem of N platforms and M architectures requires N implementations of
one interface rather than N×M toolchains. The count follows from where the
target side lives: a package built from source is built for whatever target the
compiler is asked to emit, so a platform implementation is written once and
reaches every architecture the compiler supports.

The claim is verified by a matrix of three hosts and three targets, each cell
building one source and running the result.

## What A Project Writes

```toml
[dependencies]
openkal-llvm-runtime = "0.1.1"

[toolchain]
default = "llvm@22.1.8"
```

Two lines. The first selects three layers of the target side; the second names
a compiler and says nothing about where anything else comes from.

Targets are given on the command line:

```bash
mcpp build --target x86_64-linux
mcpp build --target aarch64-macos
mcpp build --target x86_64-windows-gnu
```

No `[target.<triple>]` section is required for a hosted target, and no
preprocessor directive is required in the source. A worked example is
[examples/06-openkal-cross](../examples/06-openkal-cross).

## What The Ecosystem Supplies

| Package | Layer | Content |
|---|---|---|
| `openkal` | — | the specification, and the C++ modules that declare it |
| `openkal-linux` | `kernel-abi` | the reference implementation, on Linux system calls |
| `openkal-macos` | `kernel-abi` | on the macOS system-call surface |
| `openkal-windows` | `kernel-abi` | on Win32 and the object manager, using no C runtime symbol |
| `openkal-opensbi` | `kernel-abi` | on the RISC-V Supervisor Binary Interface, no operating system |
| `openkal-uefi` | `kernel-abi` | on UEFI Boot Services, before an operating system exists |
| `openkal-musl` | `c-abi` | musl redirected onto openkal, ported once |
| `openkal-llvm-runtime` | `compiler-runtime`, `c++-abi` | compiler-rt builtins, libunwind, libc++abi and libc++, configured for openkal-musl |

A project names the last of these. The others follow from its dependencies.

## Why The Compiler Must Be LLVM

`openkal-llvm-runtime` declares the requirement rather than leaving it to be
discovered:

```toml
requires = ["mcpp:compiler=llvm"]
```

Its sources are libc++'s, and its `std` module source in particular is compiled
by Clang. Handing that source to GCC fails inside libc++'s own headers, in a
message naming a file the reader has never opened:

```
fatal error: __config: No such file or directory
```

With the requirement declared, the build refuses the combination before it
compiles anything, and names the command that selects a compiler which satisfies
it.

## How The Target Is Chosen

The target row of mcpp's own vocabulary may carry a toolchain convention. That
convention names the payload which supplies **that target's C library**, and it
applies only when two conditions hold: the manifest states nothing for the
target, and nothing in the dependency graph supplies the target's system.

The second condition is knowable only after the graph is resolved. A project
whose C library comes from `openkal-musl` therefore keeps the compiler it asked
for, while a project with no dependencies still receives the payload the row
names. Both behaviours were measured; deciding either way in advance was wrong
for the other.

## The Environment Segment

On Linux the third segment of a target triple names the C library. Under openkal
the C library comes from the graph, so a triple that names one states a request
the graph may not honour:

```
mcpp build --target x86_64-linux-gnu     # asks for glibc
      c-abi   musl   (openkal-musl@0.3.3, graph)
```

The graph decides. Omitting the segment states no request and produces the same
artifact:

```
mcpp build --target x86_64-linux
```

The build reports the mismatch when the segment is present and disagrees. It is
a report rather than a refusal, because the segment is ignored rather than
violated. Measured on one host, `x86_64-linux` against `x86_64-linux-musl`: the
two executables differ, and after stripping they are byte-identical. What
differs is the debug information, which records the output directory, and the
directory is named after the triple. The code is the same code.

On Windows the same segment names the object ABI instead — `gnu` for PE with the
GNU ABI, `msvc` for PE with Microsoft's — and both are compatible with more than
one C library. The mismatch report is therefore scoped to platforms where the
segment names a C library.

Silence is right as a diagnostic and insufficient as a report. A reader sees

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       c-abi   musl   (openkal-musl@0.3.3, graph)
```

finds no row called `gnu`, and maps it to the nearest thing that resembles a C
library name.

Measured on the artefact of exactly that build:

| Observation | Value |
|---|---|
| imported libraries | `ntdll`, `KERNEL32`, `SHELL32` — no `msvcrt`, no `ucrtbase` |
| Itanium-mangled symbols (`_Z…`) | 4507 |
| MSVC-mangled symbols (`?…`) | 0 |

The first row is why `c-abi musl` is honest: none of MinGW's C runtime is
linked. The other two are what `gnu` selected — the Itanium C++ ABI rather than
Microsoft's.

That correspondence is to no row of the report, and the absence is the point.
The five layers record who **supplies** each layer; `gnu` names a convention the
**objects follow**, which several layers must agree on. Reading it as `c++-abi
libc++` is a second wrong answer: libc++ is one implementation of the standard
library and libstdc++ is another, and both sit on the Itanium ABI.

The report therefore names the ABI itself, whose name appears in no row and so
cannot be mistaken for one:

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu   (gnu selects the Itanium C++ ABI, not a C library)
```

The segment carries a different axis on each platform — the C library on Linux,
the object ABI on Windows, the object format where there is no operating system
— and one value records which, rather than a boolean recording only whether the
first case holds.

## Bare Metal

A target with no operating system is the same model with the platform layer
supplied by firmware rather than by a kernel. `riscv64-none-elf` over OpenSBI
runs the same source as a hosted target, including `import std`, because the
standard library it uses is the one the graph supplied rather than the
compiler's own.

Two things must be declared, both properties of the board rather than defaults:

```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

`sysroot = ""` selects the zero-libc tier. Which machine model and which
firmware mode to use are board facts, and an engine that guesses one is an
engine a different board has to fight.

A hosted cross target takes the same key with a user-mode emulator
(2026.9.2.1). An `aarch64-linux-musl` artifact built on an x86_64 host is
executed through `qemu-aarch64-static` when the project declares it, and the
package that provides the emulator is declared for the hosts that can install
it:

```toml
[xlings.workspace]
"xim:qemu-user-aarch64" = { linux = "" }

[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

Without the key, `mcpp run` reports the kernel's refusal (`Exec format error`)
and the key to write, and `mcpp test` reports every test as not run and exits
2. A host that executes the artifact natively passes `--no-runner`. The rules
are in [5 — mcpp.toml](05-mcpp-toml.md), §2.7.3.

### The Source Is The Same, The Program Is Not

"The same source" is a claim about the toolchain and the standard library, and
it holds: `import std` works, the C++ runtime is the one the graph supplied, and
no `#if` distinguishes the targets. It is not a claim that any given program
builds for any given target, and the specification is explicit about why.

A bare-metal backend provides some interfaces and not others. `openkal-opensbi`
provides `abort`, `stream`, `memory`, `env` and `time`; it provides no
filesystem and no tasks, because the machine has none. Clause 6.1 makes that
absence a link-time fact:

> An interface that an implementation does not provide is absent as a link-time
> definition, and a consumer that uses it fails to link.

A capability word therefore answers a narrower question than it first appears
to. It says how an implementation behaves *within an interface it provides* —
whether names are compared case-sensitively, what the granularity of a clock is.
Whether the interface exists at all is answered before that, by the dependency
graph, and failing that by the linker.

The distinction is easy to lose, because the query is an inline function over a
data object, so a program that merely asks whether a filesystem exists takes the
address of `kal_fs_props` and fails to link with no filesystem call anywhere in
it. Defining that word as zero in the backend removes the error and is the one
remedy the clause forbids: the program then proceeds past the point the linker
existed to stop it at. It was tried, published as `openkal-opensbi@0.1.3`, and
retracted.

### Two Routes To A Bare x86_64 Machine

An x86_64 machine with no operating system is reached in two different ways, and
the difference is what loads the program.

| Route | Target | Platform layer | Entry |
|---|---|---|---|
| UEFI application | `x86_64-windows-gnu` | `openkal-uefi` | firmware, with Boot Services available |
| Kernel, or raw bare metal | `x86_64-none-elf` | none, or `openarch` | the reset vector, with nothing beneath |

A UEFI application is PE/COFF entered through the Microsoft x64 calling
convention. Both are properties the LLVM toolchain already has, so its target is
the same triple as a Windows program and firmware function pointers are called
directly. What distinguishes it from a Windows build is which implementation of
the platform interface the graph resolved, together with three link flags that
select `IMAGE_SUBSYSTEM_EFI_APPLICATION`.

A kernel has no firmware services to call. Its target is `x86_64-none-elf`, the
zero-libc tier: no C library on the compile line, no library directory on the
link, and `#include <stdio.h>` does not resolve. The program is entered at its
own `_start` and reaches hardware directly.

`openarch` is the layer such a program builds on. It is not a platform interface
and does not answer to `mcpp:kernel-abi`; it is the architecture mechanism —
execution contexts, traps, per-CPU state and address spaces — presented as one
interface over several instruction sets, with a backend package per instruction
set. A kernel depends on it and supplies its own platform layer, or none.

### Why x86_64 Bare Metal Required Engine Work

`riscv64-none-elf` and `aarch64-none-elf` are rows in a table and nothing more:
Clang has a BareMetal toolchain for both, drives their links itself and reaches
`ld.lld`. It has none for x86_64, so that triple falls through to the generic
GCC toolchain, whose linker is the host's `g++`:

```
g++: error: unrecognized command-line option '-fuse-ld=…/ld.lld'
```

Measured for every spelling of a bare x86_64 triple, and not correctable by any
flag. The row therefore carries a linker emulation and mcpp invokes `ld.lld`
itself, which is also why the host toolchain must be shown not to participate in
such a link.

## Measured Limits

Three, recorded because each was found by building rather than by reading.

**A backend must define every capability word.** The specification's queries are
inline functions over property objects, so a program that merely asks whether a
filesystem exists takes the address of `kal_fs_props`. A backend that omits the
words for layers it lacks makes the question fail to link on exactly the class of
machine the question exists for.

**Two suppliers of one layer is an error rather than a choice.** A C library, a
platform interface and a C++ runtime are mutually exclusive. Selecting the wrong
one does not fail the link; it produces a program that runs and intermittently
does not.

**A payload's C++ runtime cannot sit above a foreign C library.** Its
`__config_site` records the configuration it was built with. The resolver's
structure prevents the combination on the default path, and a diagnostic covers
the paths where a project overrides the contract explicitly.

## Reference

[docs/14 — The Target Side](14-target-side.md) for the five layers, the four
origins and the rules. [SPEC-002](specs/target-side.md) for the normative
statement of the capability grammar.
