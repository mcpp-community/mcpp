# The Target Triple

A target triple is written `<arch>-<os>` or `<arch>-<os>-<env>`. This chapter
states what each segment means, when the third may be declined, and why the
answer differs between two systems that mcpp supports at the same time.

## Two Systems, One Spelling

mcpp resolves the target side — the platform interface, the C library, the
compiler runtime and the C++ runtime — in one of two ways, and a project
usually uses one of them without choosing explicitly.

**The prebuilt system.** A toolchain payload is built for one target and
carries that target's C library with it. Selecting `x86_64-linux-musl` selects
the musl-gcc payload; selecting `x86_64-linux-gnu` selects a glibc one. The
triple's third segment is load-bearing at resolution time, because it is how
the payload is chosen.

**The build-time system.** The target side arrives as packages in the
dependency graph and is compiled from source by whichever compiler is running.
The third segment selects nothing, because the graph has already decided. This
is what [chapter 15](15-openkal-cross.md) describes.

The two differ in what the third segment *does*, not in how it is spelled. A
project does not declare which system it is in; the dependency graph decides,
and the build reports what it resolved.

## The Segments

| Segment | Content | Example |
|---|---|---|
| `arch` | instruction set | `x86_64`, `aarch64`, `riscv64` |
| `os` | operating system, or `none` | `linux`, `windows`, `macos`, `none` |
| `env` | see below — it is a different axis per platform | `gnu`, `musl`, `msvc`, `elf` |

The third segment is the one that repays attention, because it does not name
the same kind of thing everywhere:

| Platform | `env` names | Values |
|---|---|---|
| `linux` | the **C library** | `gnu` (glibc), `musl` |
| `windows` | the **object ABI** | `gnu` (Itanium C++ ABI), `msvc` (Microsoft's) |
| `none` | the **object format** | `elf` |
| `macos` | nothing; the platform carries no segment | — |

On Windows the segment is frequently misread, because the word `gnu` suggests a
C library that is not there. Measured on an artefact built for
`x86_64-windows-gnu` over the build-time system:

| Observation | Value |
|---|---|
| imported libraries | `ntdll`, `KERNEL32`, `SHELL32` — no `msvcrt`, no `ucrtbase` |
| Itanium-mangled symbols (`_Z…`) | 4507 |
| MSVC-mangled symbols (`?…`) | 0 |

Nothing GNU is present: the compiler is clang, the linker lld, the compiler
runtime compiler-rt, the C library musl, the C++ runtime libc++, the platform
openkal. `gnu` is LLVM's label for the non-MSVC ABI, inherited from MinGW, and
clang requires that spelling to select the right internal toolchain. mcpp
cannot rename it.

## Declining The Third Segment

`<arch>-<os>` is a complete target on every platform:

```bash
mcpp build --target x86_64-linux      # = x86_64-linux-gnu
mcpp build --target x86_64-windows    # = x86_64-windows-gnu
mcpp build --target riscv64-none      # = riscv64-none-elf
mcpp build --target aarch64-macos     # macOS has no segment to decline
```

Declining it changes no identity. The output directory, the cache key and the
subject of a `cfg()` predicate are all the canonical form, so the two spellings
share one fingerprint and the second build is a cache hit rather than a second
full build.

What differs is the record of what was asked for. A triple serves as an
identity, which must be total, and as a request, which must be able to say
nothing; mcpp keeps both, filling the segment for the identity while recording
that the fill was a fill.

### Which Spelling To Use

**Under the build-time system, decline it.** The graph supplies the C library
and the runtimes, so the segment states a request that is not consulted. Under
this system `x86_64-windows` is not merely shorter than `x86_64-windows-gnu` —
it is more accurate, because nothing GNU is involved.

**Under the prebuilt system, write it when the choice matters.**
`x86_64-linux-musl` and `x86_64-linux-gnu` select different payloads and produce
different artefacts. Writing the segment is how that choice is made.

**On Windows, write `msvc` when Microsoft's ABI is wanted.** `gnu` is the
default fill, and `msvc` is a different object ABI rather than a different C
library, so the segment is meaningful there in both systems.

## What The Build Reports

The report heads with the target as written and resolves it to the compiler's
own spelling:

```
      Target x86_64-windows → x86_64-w64-windows-gnu
             kernel-abi        openkal        (openkal-windows@0.1.3, graph)
             c-abi             musl           (openkal-musl@0.3.3, graph)
             c++-abi           libc++         (openkal-llvm-runtime@0.1.2, graph)
```

Two diagnostics attach to the third segment, and which one applies follows from
the table above.

**Where the segment names a C library and the graph supplies a different one,
the build reports the disagreement.** The graph decides, so this is a report
rather than a refusal — the artefact is the same either way, and only the name
is inaccurate:

```
warning: the target name asks for the `gnu` C ABI and the dependency graph supplies `musl`.
       The graph decides, so the build below uses `musl` — the name is what is inaccurate,
       not the artifact. Drop the segment to say what is actually meant:
           --target x86_64-linux
```

**Where the segment names something else, the report says what.** A warning
would be wrong there: it would fire on every legitimate MinGW build and would
describe an axis the name never addressed. The gloss appears only when the
segment was written out, and names the ABI itself rather than any layer:

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu   (gnu selects the Itanium C++ ABI, not a C library)
```

That correspondence is to no row of the report, and the absence is the point.
The five layers record who **supplies** each layer; the segment names a
convention the **objects follow**, which several layers must agree on. Reading
it as `c++-abi libc++` is a second wrong answer, since libstdc++ sits on the
same ABI.

## Custom Targets

A triple outside mcpp's table needs an explicit section, which is also how a
board declares facts no default can supply:

```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

`sysroot = ""` selects the zero-libc tier: no C library on the compile line and
none on the link. An absent `sysroot` key is a different answer — it inherits
the target row's own default. See [chapter 13](13-baremetal.md).

## Reference

[chapter 14](14-target-side.md) for the five layers and who supplies them.
[chapter 15](15-openkal-cross.md) for the build-time system in full.
[chapter 03](03-toolchains.md) for the toolchain axis, which is separate: a
target does not determine a compiler.
