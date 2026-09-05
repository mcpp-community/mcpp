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
mcpp build --target aarch64-linux     # = aarch64-linux-musl
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

### The completion is chosen from the vocabulary, not from a fixed word

The fourth line above is why the two roles have to stay separate. Filling
`aarch64-linux` lexically gives `aarch64-linux-gnu`, and that row is `planned`
— while `aarch64-linux-musl` is `verified`. Before 2026.8.26.2 the tier gate
asked about the filled value, so:

```
$ mcpp build --target aarch64-linux
  error: target 'aarch64-linux-gnu' is registered but not yet supported (planned)
$ mcpp build --target aarch64-linux-musl
  Finished dev [unoptimized + debuginfo] in 0.99s
```

The question asked was *aarch64, Linux*. The question answered was
*aarch64-linux-**gnu***, and the message quotes a triple that appears nowhere in
the command. `riscv64-linux` was worse: the fill named a row outside the
vocabulary entirely, so a registered family was reported as `unknown target`.

A request that declined the segment is completed against the known-target table,
in this order:

1. the lexical default names a supported row — take it (`x86_64-linux` → `gnu`);
2. exactly one row for this `(arch, os)` is supported — take it
   (`aarch64-linux` → `musl`);
3. nothing is supported — keep the lexical form, and diagnose against the rows
   that *do* exist (`riscv64-linux` → "planned; registered rows for this system:
   `riscv64-linux-musl`");
4. several are supported and the lexical default is none of them — refuse and
   list them. No `(arch, os)` has this shape today.

Rule 1 comes first so this retires itself: the day `aarch64-linux-gnu` graduates
from `planned`, the lexical answer wins again with nothing to edit.

**Writing the segment opts out.** A written segment is a request, not a gap, so
`--target aarch64-linux-gnu` still reaches the `planned` row's refusal — which
is the escape hatch for opting into a row early with an explicit
`[target.<triple>] toolchain`.

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

## Three Vocabularies, And Why They Differ

A triple is written by three parties that do not share a convention, and mcpp
translates between them. Knowing which one a string belongs to removes most of
the confusion the third segment causes.

### The shape

```
<arch> - <vendor> - <os> - <env>
```

Every field may be omitted and the compiler fills what is missing. `vendor` is
historical and carries almost nothing today — it is filled as `unknown` unless
the target has a reason to say otherwise:

```
x86_64-linux-gnu   →  x86_64-unknown-linux-gnu
aarch64-macos      →  aarch64-unknown-macos
```

A three-field spelling is a four-field one with a field elided, and which field
was elided follows from what parses.

### GCC and clang do not select targets the same way

| | GCC | clang |
|---|---|---|
| target | fixed when the compiler was **built** | chosen at **run time** |
| how to ask | `-dumpmachine` | `-dumpmachine`, or `--target=` |
| to cross-compile | use a **different executable** | pass a flag |

Measured:

```
g++                     →  x86_64-linux-gnu       (emits nothing else)
x86_64-w64-mingw32-g++  →  x86_64-w64-mingw32     (emits nothing else)
clang++                 →  x86_64-unknown-linux-gnu, and --target= changes it
```

This is why the prebuilt system passes no `--target`: the payload's compiler
is named after the one target it can emit, and choosing a target means choosing
a payload. It is also why the build-time system needs only one compiler.

### MinGW names itself in the GCC convention

MinGW's own triple is `x86_64-w64-mingw32`:

| Field | Value | Why |
|---|---|---|
| arch | `x86_64` | |
| vendor | `w64` | the project is `mingw-w64`, distinguishing it from the stalled original `mingw32` |
| os | **`mingw32`** | MinGW puts *itself* in the OS field |
| env | (absent) | three fields is the whole name |

The convention descends from autoconf's `config.guess`, where the OS field names
the target's runtime environment — and in the GNU toolchain's view MinGW *is* a
distinct environment, with its own headers, its own C runtime and its own
`configure` branches. The `32` is historical; `w64` is what says the name refers
to the 64-bit-capable project.

LLVM does not accept that view. It holds that the OS is `windows` and that MinGW
is one ABI environment on top of it, so it re-spells the name — measured:

```
x86_64-w64-mingw32  →  x86_64-w64-windows-gnu
x86_64-pc-mingw32   →  x86_64-pc-windows-gnu
```

```
GCC   x86_64 - w64     - mingw32 - (none)
                ^vendor   ^os
LLVM  x86_64 - unknown - windows - gnu
                ^vendor   ^os       ^env
```

**`mingw32` is split out of the OS field into `windows` plus `gnu`.** The
value `gnu` exists because LLVM needed a name for the half that was left over.
Its meaning is "the MinGW/Itanium ABI lineage" and it has never meant "the C
library is glibc" on Windows — the same word carries different duties under
different operating systems, and that is LLVM's vocabulary rather than mcpp's
invention.

### What mcpp keeps

| Vocabulary | Example | Read by |
|---|---|---|
| GCC / autoconf | `x86_64-w64-mingw32` | the prebuilt payload's compiler, by its file name |
| LLVM | `x86_64-w64-windows-gnu` | `clang --target=` |
| **mcpp** | `x86_64-windows-gnu` | the target table, the output directory, `cfg()`, a packed ABI tag |

mcpp's own form must map onto both. The arrow in the build report is that
mapping, from the third vocabulary to the second:

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       ^ mcpp                ^ LLVM
```

Keeping the third vocabulary separate is what lets mcpp name something LLVM
cannot. Measured on llvm 22.1.8, `windows` with a `musl` environment is accepted
by the triple parser and crashes the compiler:

```
clang++ --target=x86_64-pc-windows-musl -c t.cpp
    #5  llvm::MCWinCOFFStreamer::emitCGProfileEntry(...)
```

The crash is in the COFF writer: clang has decided the output is COFF and has no
Windows-musl path to configure the streamer with. Of the four non-MSVC Windows
environments it knows — `gnu`, `cygnus`, `itanium`, `musl` — the first three
compile and only `musl` dies, and the predefined macros show why the environment
was never modelled:

| Triple | Macros |
|---|---|
| `…-windows-gnu` | `__GNUC__` `__MINGW32__` `_WIN32` |
| `…-windows-msvc` | `_MSC_VER` `_WIN32` |
| `…-windows-musl` | `__GNUC__` `_WIN32` — **no `__MINGW32__`** |

So a musl C library on Windows cannot be named to clang, and must still be named
by mcpp, because mcpp's name answers a different question: **which C library**,
where LLVM's answers **which object ABI**. Both are needed and they are not the
same string.

## The Compiler And The C Library Are Two Axes

A target names a machine. It does not name who compiles for it, and it does not
name where its C library comes from — those are two further choices, and the
same target string means a different build under each.

**"From the payload" is not one payload.** It is whichever payload the
chosen compiler brings, and gcc and clang bring them differently: gcc has one
payload per target, with a triple-prefixed driver, while one `clang++` emits
every target it was built with. Measured on one host, one source:

| toolchain | target | driver that ran | c-abi | c++-abi |
|---|---|---|---|---|
| `gcc@16.1.0` | `x86_64-linux-musl` | `xim-x-musl-gcc/…/x86_64-linux-musl-g++` | musl | libstdc++ |
| `gcc@16.1.0` | `x86_64-windows-gnu` | `xim-x-mingw-cross-gcc/…/x86_64-w64-mingw32-g++` | gnu | libstdc++ |
| `llvm@22.1.8` | `x86_64-linux-musl` | `xim-x-llvm/…/clang++` | musl | libc++ |
| `llvm@22.1.8` | `x86_64-windows-gnu` | `xim-x-llvm/…/clang++` | gnu | libc++ |

clang does not reach into gcc's payload for a C library, and gcc does not reach
into clang's. Each brings its own.

### Choosing the other compiler means supplying the other C library

Each hosted row names a toolchain, and that name is a convention rather than a
capability: it answers *which payload supplies this target's C library*, so a
project that supplies one itself may name a different compiler. What it may not
do is name a different compiler and supply nothing.

```toml
[toolchain]
default = "llvm@22.1.8"        # x86_64-linux-musl's row names gcc
```

```
$ mcpp build --target x86_64-linux-musl
error: target 'x86_64-linux-musl' takes its C library from the 'gcc@16.1.0'
       payload, and 'llvm@22.1.8' has none here.
```

**Before 2026.8.26.1 this ran the whole build and failed at the link**, with
`crtbeginT.o (bare name — the linker cannot resolve it)` — accurate about the
symptom and silent about the decision. clang is retargetable and brings no C
library, so it reached for a gcc installation; on a machine that happens to have
a system one, the same spelling for `x86_64-windows-gnu` reached
`/usr/lib/gcc/x86_64-w64-mingw32/…` instead, which is worse than failing.

Adding the replacement is the whole difference:

```toml
[dependencies]
openkal-llvm-runtime = "0.1.3"   # → openkal-musl → openkal-<os>
[toolchain]
default = "llvm@22.1.8"
```

That is [`examples/06-openkal-cross`](../examples/06-openkal-cross), and it is
why the refusal names openkal in its own text.

**A bare-metal row's toolchain is not a convention, and neither is
`x86_64-windows-musl`'s** — no gcc payload emits a PE with a musl C library, so
those rows cannot be overridden at all. See [chapter 03](03-toolchains.md).

### And a dependency graph replaces that axis entirely

The same three targets, with `openkal-musl` and `openkal-llvm-runtime` in the
graph — measured the same way:

| target | kernel-abi | c-abi | c++-abi |
|---|---|---|---|
| `x86_64-linux-musl` | openkal (openkal-linux, graph) | musl (graph) | libc++ (graph) |
| `x86_64-windows-gnu` | openkal (openkal-windows, graph) | musl (graph) | libc++ (graph) |
| `x86_64-windows-musl` | openkal (openkal-windows, graph) | musl (graph) | libc++ (graph) |

**Look at `x86_64-windows-gnu` in both tables.** Its C library is `gnu` — the
MinGW CRT — when a payload supplies it, and `musl` when the graph does. One
target string, two different C libraries, and until 2026.8.24.6 mcpp had no way
to say which: the same `--target x86_64-windows-gnu` produced artefacts that
differed by 16.7× in size and named entirely different DLLs.

That is why `x86_64-windows-musl` exists as a separate name. It maps to the same
LLVM triple as `x86_64-windows-gnu` — LLVM cannot spell it — so the two are
indistinguishable to the compiler, and the whole difference is which C library
is in use. A payload for it does not exist on any host; its system can only come
from a dependency graph, which is what `toolchain list` reports as
`via dependency graph`.

## The Build Host Is A Third Axis

The two axes above — which compiler, and where the C library comes from — are
choices a project makes. The third is not: it is the machine the build runs on.

**The axis is the set mcpp ships for, and it is (os, arch) rather than os.**
`release.yml` publishes four host binaries:

| build host | release asset | CI runner |
|---|---|---|
| `linux-x86_64` | `mcpp-<v>-linux-x86_64.tar.gz` | `ubuntu-24.04` |
| `linux-aarch64` | `mcpp-<v>-linux-aarch64.tar.gz` | `ubuntu-24.04-arm` |
| `macos-arm64` | `mcpp-<v>-macosx-arm64.tar.gz` | `macos-14` |
| `windows-x86_64` | `mcpp-<v>-windows-x86_64.zip` | `windows-2022` |

**The two Linux hosts are not one host.** `x86_64-linux-gnu` needs the
host-native `xim:glibc` and `xim:linux-headers` payloads, which exist for the
host's own architecture only — so that row is reachable from `linux-x86_64` and
not from `linux-aarch64`, while `aarch64-linux-gnu` is the mirror case and is
`planned` on both. Collapsing them to `linux` would let one overwrite the
other's rows.

### Which build host serves which target

| target | tier | pin | linux-x86_64 | linux-aarch64 | macos-arm64 | windows-x86_64 |
|---|---|---|---|---|---|---|
| `x86_64-linux-gnu` | verified | — | payload | — | — | — |
| `aarch64-linux-gnu` | planned | — | planned | planned | planned | planned |
| `x86_64-linux-musl` | verified | `gcc@16.1.0` | payload | payload | — | payload |
| `aarch64-linux-musl` | verified | `gcc@16.1.0` | payload | payload | — | — |
| `riscv64-linux-musl` | planned | — | planned | planned | planned | planned |
| `x86_64-windows-gnu` | verified | `gcc@16.1.0` | payload | payload | — | payload |
| `x86_64-windows-musl` | preview | `llvm@22.1.8` | graph | graph | graph | payload |
| `x86_64-windows-msvc` | verified | — | — | — | — | system |
| `aarch64-macos` | verified | — | — | — | SDK | — |
| `x86_64-macos` | planned | — | planned | planned | planned | planned |
| `riscv64-none-elf` | verified | `llvm@22.1.8` | payload | payload | payload | payload |
| `riscv32-none-elf` | verified | `llvm@22.1.8` | payload | payload | payload | payload |
| `aarch64-none-elf` | preview | `llvm@22.1.8` | payload | payload | payload | payload |
| `x86_64-none-elf` | preview | `llvm@22.1.8` | payload | payload | payload | payload |
| `thumbv6m-none-eabi` | verified | `llvm@22.1.8` | payload | payload | payload | payload |
| `thumbv7m-none-eabi` | verified | `llvm@22.1.8` | payload | payload | payload | payload |
| `thumbv7em-none-eabi` | preview | `llvm@22.1.8` | payload | payload | payload | payload |
| `thumbv7em-none-eabihf` | verified | `llvm@22.1.8` | payload | payload | payload | payload |
| `thumbv8m.base-none-eabi` | preview | `llvm@22.1.8` | payload | payload | payload | payload |
| `thumbv8m.main-none-eabi` | verified | `llvm@22.1.8` | payload | payload | payload | payload |
| `thumbv8m.main-none-eabihf` | preview | `llvm@22.1.8` | payload | payload | payload | payload |

`payload` a toolchain payload here produces it · `graph` no payload, but a
dependency can supply the system · `system` located on the machine, not
installed by mcpp · `SDK` the platform's own · `—` unreachable from this host
· `planned` registered in the vocabulary, nothing wired yet.

### The rule behind the columns

| target class | which build hosts serve it | why |
|---|---|---|
| `*-linux-musl` | Linux (any arch), Windows (same arch only) | the musl payloads are self-contained |
| `*-linux-gnu` | Linux, same arch only | additionally needs the host-native `xim:glibc` / `xim:linux-headers` |
| `x86_64-windows-gnu` | Linux, Windows | one identity, host-split at the distribution layer |
| `x86_64-windows-msvc` | Windows | MSVC is located on the machine |
| `x86_64-windows-musl` | Windows by payload; anywhere by graph | no gcc emits PE+musl, and LLVM cannot spell the triple |
| `aarch64-macos` | macOS | the SDK is the machine's |
| `*-none-elf` | every host | clang and lld are cross-compilers by construction |

**A `—` is about payloads, not about possibility.** `host_can_serve` answers
"does a payload here produce it", and a dependency graph can supply the system
instead — which is why `x86_64-windows-musl` reads `via dependency graph` on
Linux and produces a real PE32+ there.

### And CI measures every one of them

[`ci-target-matrix.yml`](../.github/workflows/ci-target-matrix.yml) runs on all
four hosts. Each scans every row it lists, twice — the payload alone, then with
`openkal-musl` + `openkal-llvm-runtime` in the graph — and compares against
[`tests/matrix/expected.tsv`](../tests/matrix/expected.tsv), which is keyed on
`(mode, host, target, compiler)`.

What a runner resolves as its own target is not what its name suggests:

| runner | host target it resolves |
|---|---|
| `ubuntu-24.04` | `x86_64-unknown-linux-gnu` |
| `ubuntu-24.04-arm` | `aarch64-unknown-linux-gnu` |
| `macos-14` | `arm64-apple-darwin23.6.0` — **ARM**, not x86_64 |
| `windows-2022` | `x86_64-pc-windows-msvc` — **msvc**, while a mingw gcc there targets `-gnu` |

Three of this chapter's criteria assumed the Linux coincidence and had to be
corrected on the other hosts.

**The cell count per host is not a constant.** It follows from what that
machine has installed, and the same runner has been measured with different
toolchains on consecutive runs. The comparison therefore asserts *the scan
produced rows* and *every row the expected table names was reached*, rather than
a total: a cell that vanishes because a payload was not restored is otherwise
indistinguishable from a cell that passed.

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
