# 13 — Bare-Metal and Freestanding Targets

This document describes how mcpp builds, runs and tests software for targets
with no operating system underneath, and how a board-support package supplies
the parts of such a target that the engine deliberately does not know.

Related documents: [05 — mcpp.toml Manifest Guide](05-mcpp-toml.md) §2.7.2 is
the reference for the `[target.<triple>]` keys used here; [07 — build.mcpp
Build Program](07-build-mcpp.md) is the reference for the directive protocol a
board-support package speaks; [08 — Toolchain Internals](08-toolchain-internals.md)
covers the hosted link model this chapter departs from.

## Overview

A freestanding target is a target whose `os` field is `none`. The target table
at `modules/toolchain-model/src/triple.cppm` carries four of them:

| Triple | Tier | C library |
|---|---|---|
| `riscv64-none-elf` | verified | `xim:picolibc-riscv` |
| `riscv32-none-elf` | verified | `xim:picolibc-riscv` |
| `aarch64-none-elf` | preview | none by default — the zero-libc tier; `xim:picolibc-aarch64` is declarable |
| `x86_64-none-elf` | preview | none by default — the zero-libc tier; `xim:picolibc-x86` is declarable |

`verified` means an image has been built **and run** for the row. `preview`
means it builds and has been observed to run, but is not yet covered by the
engine's own emulator jobs.

⚠️ The last two rows default to no C library, and that is a statement rather
than an omission: the first consumer of both rows — the `openarch` layer of
machine mechanism — references no C library symbol, and if no row defaulted to
this tier there would be nothing demonstrating the tier works.
⭐ **A build for those rows is declarable, not absent** (mcpp 2026.8.21.3+).
`xim:picolibc-aarch64` and `xim:picolibc-x86` are in the index; a project that
wants one names it the same way it would choose a different one:

```toml
[target.aarch64-none-elf]
sysroot = "xim:picolibc-aarch64@1.8.12"
``` An empty column means exactly what
`[target.<triple>].sysroot = ""` means in a manifest, so a project targeting one
begins on the zero-libc tier without asking. A project that wants a C library on
those targets declares one, which is also how it would choose a different one.

Such a target needs no per-host cross toolchain. clang and lld are
cross-compilers by construction — one binary emits every target it was built
with — so the target table pins `llvm@22.1.8` on every host, and any machine
that can install the LLVM payload can produce an image for any of the four.

### The x86_64 row is not four strings

⚠️ **A target row is normally an entry in two tables and nothing else. This one
needed engine code, and the reason is a property of clang rather than of the
instruction set.**

clang selects a toolchain from the triple. It has a *BareMetal* toolchain for
arm, aarch64 and riscv, which links with `ld.lld` directly; it has none for
x86_64, so every spelling of a bare x86_64 triple falls through to the generic
GCC toolchain — whose linker is the **host's `g++`**:

```
g++: error: unrecognized command-line option '-fuse-ld=/…/llvm/22.1.8/bin/ld.lld'
```

Measured for `x86_64-none-elf`, `x86_64-unknown-none-elf`, `x86_64-unknown-none`,
`x86_64-elf`, `x86_64-none-none` and `x86_64-unknown-unknown`, and unchanged by
`-fuse-ld=lld`, `--ld-path=`, `--gcc-toolchain=` or `-B`. The one thing that
does change it is putting `linux` in the OS position, which makes clang link
directly and adds eight host `-L` paths to a bare-metal link.

Neither outcome is acceptable: routing through a host `g++` makes the row work
on a Linux host and nowhere else, and host search paths on a freestanding link
are the hermeticity this engine exists to keep. So the row carries a fifth
column, `lldEmulation`, and when it is set the engine drives the link with
`ld.lld` itself. The flag vocabulary changes with the tool — `-Map=` rather than
`-Wl,-Map=`, `-m elf_x86_64` rather than `--target=` — and the driver-only flags
(`-nostdlib++`, the loader tag) are dropped rather than translated.

The column is empty for the riscv and aarch64 rows. Their driver already reaches
lld, and changing a working link to make three rows look alike is how a
regression is introduced.

### `-mno-red-zone` is part of the target, not a preference

The System V x86-64 ABI reserves 128 bytes below `rsp` that a leaf function may
use without adjusting the stack pointer, because on a hosted system nothing else
writes there. On bare metal the processor pushes an interrupt frame at `rsp` —
into the red zone — and the interrupted leaf resumes to find its locals
overwritten. There is no fault and no diagnostic, and it happens only when an
interrupt arrives inside a leaf.

There is no bare-metal x86_64 program for which the red zone is safe, so the
flag is a property of the row rather than something a project remembers. It
reaches the command line through a new `extra` column in the ISA-profile table,
which exists because `-march`/`-mabi`/`-mcmodel` could not express it. RISC-V
and aarch64 have no equivalent, which is why the column did not exist before.

Three things a bare-metal build requires are not properties of the ISA, and
mcpp does not attempt to derive them: which startup object and libraries to
select, which linker script describes the machine's memory, and how to execute
the resulting image. These travel with a **board-support package**, an ordinary
dependency. The consequence is that changing boards is a dependency change
rather than a build-system change.

## The shortest path to a running image

Two commands produce a booting image, with no linker script, load address or
emulator invocation written by hand:

```bash
mcpp new blinky --template riscv-virt-rt
cd blinky
mcpp run
```

Measured output:

```
   Resolving toolchain
    Resolved llvm@22.1.8 → riscv64-none-elf → @mcpp/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/clang++
    Resolved host toolchain for build.mcpp: clang 22.1.8 (x86_64-unknown-linux-gnu)
  build.mcpp compiling
  build.mcpp running
    Inferred sources [src/**/*.{cppm,cpp,cc,c,S,s,asm}]
    Inferred target blinky (bin from src/main.cpp)
   Compiling blinky v0.1.0 (.)
      Cached riscv-virt-rt v0.3.0 (1 unit)
    Finished dev [unoptimized + debuginfo] in 0.05s
        Size blinky  text 8572  data 80  bss 5668  total 14320
     Running `…/xim-x-qemu-riscv/9.2.4-1/bin/qemu-system-riscv64 … target/riscv64-none-elf/…/bin/blinky`

hello from blinky
float 3.1416
heap ok
```

The `Size` line is printed after every freestanding link. Capacity is the
governing constraint on a bare-metal target, and the number is already known
the moment the link finishes; omitting it would require every project to run
`size` separately. It is silent on hosted targets and whenever the tool is
absent, because an informational line has no standing to fail a build.

### The generated project

The entire manifest is four declarations:

```toml
[package]
name    = "blinky"
version = "0.1.0"

[build]
target = "riscv64-none-elf"

[dependencies]
riscv-virt-rt = "0.3.0"
```

There is no `[target.*]` section, no linker script path, no load address, no
`-nostdlib`, no `-march`/`-mabi`/`-mcmodel`, no crt0, no C library name and no
emulator command line. The ISA flags come from the engine's target table; the
remainder comes from the board-support package named in `[dependencies]`.

The generated `src/main.cpp` is an ordinary `main`:

```cpp
import mcpplibs.riscv_virt_rt;

extern "C" int main() {
    board::println("hello from blinky");
    board::printf("float %.4f\n", 3.14159);
    void* p = board::alloc(64);
    board::println(p ? "heap ok" : "heap FAILED");
    board::release(p);
    return p ? 0 : 1;
}
```

No `_start` and no assembly entry point are required, because the board-support
package selects picolibc's semihosting `crt0` — the C runtime is initialised
before `main` runs, and the return value reaches the host through semihosting.
Only a board with no C library at all needs an explicit entry point, declared
by pointing `main` at the source file that carries `_start`.

## What a freestanding target changes

| Aspect | Behaviour on a freestanding target |
|---|---|
| Link line | Built from nothing rather than extended: `-nostdlib -nostartfiles -static`, with no crt files, no dynamic linker and no C++ runtime. Appending `-nostdlib` to a hosted line would rely on the driver discarding the earlier flags in the right order. |
| Linker selection | `ld.lld` is addressed by **absolute path**, derived from the driver's own directory. `-fuse-ld=lld` resolves by name and finds GNU ld on any machine with binutils earlier on `PATH`, which then fails with `unrecognised emulation mode: elf64lriscv`. |
| ISA flags | `-march`, `-mabi` and `-mcmodel` come from one row per target in `src/freestanding/target.cppm`, so `--target <triple>` alone is sufficient to produce a correct object file. |
| C library | The **target's**, resolved by mcpp from the target's own table row exactly as the compiler is. A bare-metal project declares no libc, just as a hosted project declares no glibc. The engine places the sysroot's library directory on the link search path, so a board-support package selects out of it by bare name (`-lc`, `-lcrt0-semihost`). |
| Exceptions and RTTI | Off on every translation unit in the graph, including a dependency's. There is no unwinder and no `libc++abi`, so nothing can throw; `std::optional::value()` alone would otherwise reference `__cxa_throw` and three further undefined symbols. The setting belongs to the target rather than to a project's `cxxflags` because a BMI records it, and a dependency compiled with exceptions cannot be imported by a unit without them. |
| `import std` | Unavailable, and rejected at configure time with a diagnostic rather than at link time. |
| Entry point | `int main()` is available whenever something supplies a `crt0`. A board-support package normally does. |
| Default linkage | Static, and not as a preference: there is no loader, so there is no other option. |

A project with no dependencies at all still builds, which is the evidence that
the ISA row alone is sufficient:

```
        Size norunner  text 12  data 0  bss 0  total 12
```

## The layering: engine, target and board-support package

The division of responsibility is a single sentence: **location is a target
fact, selection is a board fact.**

| Layer | Owns | Example |
|---|---|---|
| Engine | The ISA profile, the freestanding link line, the artifact set, the single read point for how an artifact is executed | `-march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding` |
| Target | Which compiler and which C library, both resolved from the target's row and installed on demand | `pin = llvm@22.1.8`, `sysroot = xim:picolibc-riscv@1.8.12` |
| Board-support package | Which startup object and libraries to select, which linker script, which emulator invocation | `-lcrt0-semihost`, `picolibcpp.ld`, `qemu-system-riscv64 -machine virt …` |

The middle row is what keeps a package from having to name a C library.
Earlier versions of both ecosystem packages declared
`[xlings] deps = ["xim:picolibc-riscv@1.8.12"]`, which bound a package to one
libc, one architecture and one compiler implementation. That declaration is no
longer required, and the target's sysroot column replaced it.

A second board on the same ISA is a change of the three values in the bottom
row. It requires no change to the engine.

## Worked examples

Every transcript in this section was measured with mcpp 2026.8.20.1 on
`x86_64-linux-gnu`; see [Verification scope](#verification-scope).

### Switching ISA width

The same sources and the same board-support package serve both widths:

```bash
mcpp run --target riscv32-none-elf
```

```
        Size blinky  text 10412  data 48  bss 5400  total 15860
hello from blinky
float 3.1416
heap ok
```

Neither the project's sources nor the board-support package changed. The
package selects its profile from `MCPP_TARGET_ARCH`, and the ISA parameters
come from the engine's table, which is data rather than code — supporting a
further width is one row.

### The freestanding standard-library subset

`import std` is one module over the entire library, threads, filesystem and
iostreams included, so there is no subset of it to build without an operating
system. An ordinary dependency carries the parts that need no OS:

```toml
[dependencies]
riscv-virt-rt    = "0.3.0"
std-freestanding = "0.2.0"
```

```cpp
import mcpplibs.riscv_virt_rt;
import mcpplibs.std.freestanding;      // not `import std;`

struct Task { int prio; const char* name; };

extern "C" int main() {
    std::array<Task, 4> tasks{{ {3,"c"}, {1,"a"}, {4,"d"}, {2,"b"} }};
    std::ranges::sort(tasks, {}, &Task::prio);
    for (const auto& t : tasks) board::printf("%s", t.name);
    board::println("");

    std::optional<int> o = 41;
    std::atomic<int>   a{0};
    a.fetch_add(o.value() + 1);
    board::printf("atomic %d\n", a.load());

    std::span<Task>  s{tasks};
    std::string_view sv{"ok"};
    board::printf("span %zu %s\n", s.size(), sv.data());
    return 0;
}
```

Measured output and size:

```
        Size blinky  text 19564  data 72  bss 5632  total 25268

abcd
atomic 42
span 4 ok
```

The subset covers 103 of the 110 `std/*.inc` headers the LLVM 22.1.8 payload
ships — counted in both trees on 2026-08-20 — and it is generated by mechanical
selection rather than written as an export list. The 7 it omits are reported by
the package to fail on a hosted `x86_64` as well; that report was not
re-measured here. Available entities include `array`, `span`, `optional`, `expected`,
`atomic`, `string_view`, `ranges`, `algorithm`, `bit`, `charconv`, `concepts`,
`type_traits`, `tuple`, `utility` and coroutines.

What the subset excludes is excluded at compile time rather than at run time:

```cpp
std::mutex m;
```

```
error: no type named 'mutex' in namespace 'std'
```

### The allocating half of the subset

The subset does not change what compiles. All of its headers are included
unconditionally and `std::vector` compiles today; what fails is the **link**,
because a freestanding target has no compiled `libc++` and therefore no
`operator new`:

```
ld.lld: error: undefined symbol: operator new(unsigned long)
>>> referenced by allocate.h:58
```

| Needs nothing | Needs an allocator |
|---|---|
| `array` `span` `optional` `expected` `atomic` `string_view` `ranges` `algorithm` `bit` `charconv` `tuple` | `vector` `string` `deque` `list` `map` `set` `unordered_*` `function` `any` `make_unique`, and the default coroutine frame |

An allocator arrives with a feature:

```toml
[dependencies]
riscv-virt-rt    = "0.4.0"
std-freestanding = { version = "0.3.0", features = ["alloc-libc"] }
```

```
        Size blinky  text 13764  data 80  bss 5668  total 19512

vector 5 last=16
```

`alloc-libc` forwards to the target's C library and is the shorter path when the
target has one. `alloc-kal` forwards to openkal instead, which is what a project
wants when the same sources must also build for a target whose environment is
not a C library; on bare metal the board package supplies the openkal backend,
because the console and the heap region are board facts:

```toml
riscv-virt-rt    = { version = "0.4.0", features = ["openkal"] }
std-freestanding = { version = "0.3.0", features = ["alloc-kal"] }
```

`operator new` is a whole-program singleton, so the implementation is a separate
package and the choice belongs to the program. The feature states the
requirement as a capability; the implementations provide it; the resolver binds
exactly one. Both failure modes are therefore reported when the graph resolves,
naming packages rather than mangled symbols:

```
error: no package provides capability 'freestanding-allocator' required by 'std-freestanding'

error: capability 'freestanding-allocator' has multiple providers in the graph:
       [std-freestanding-alloc-kal, std-freestanding-alloc-libc]
```

### A target with no C library

`[target.<triple>].sysroot` overrides the C library the target table binds, on
the same axis as `toolchain` overriding the compiler pin. The empty string
declines a C library altogether:

```toml
[target.riscv64-none-elf]
sysroot = ""
```

With that line the C headers leave the compile line and the C library leaves the
link. `#include <stdio.h>` stops resolving, and the image contains only what the
project and its dependencies put in it. Measured: a self-contained image with
its own entry point and linker script links at **108 bytes** and boots.

An absent key and an empty one are different answers. Absent inherits the target
table's C library; present-and-empty declines it. A kernel or a bootloader wants
the second, and

```bash
mcpp new mykernel --template riscv-virt-rt:nolibc
```

produces a project already in that arrangement — an entry point, a memory map
and a device, measured at 369 bytes.

Four of the C functions a freestanding translation unit still reaches for are an
obligation rather than a convenience: `memcpy`, `memmove`, `memset` and `memcmp`
must exist because the compiler lowers structure assignment and array
initialisation onto them. `std-freestanding-nolibc` supplies those and `strlen`.

The subset composes with this tier rather than excluding it. `std-freestanding`
with `features = ["nolibc"]` compiles against no C library at all, and measured,
**94 of its 103 headers** do. The obstacle was never that the subset wants a C
library: libc++ ships wrappers for the C headers — `string.h` and its siblings —
which reach the real header through `#include_next` to obtain `size_t`,
`mbstate_t`, `time_t` and `EOF`. With no C library the chain has nothing to
continue to, and the wrapper fails on a missing *type* rather than a missing
header, which is why the cause is not evident from the error. Four small headers
restore the chain, and the feature is what pulls them in.

A board-support package can serve this tier too, and for the same reason it is
worth having one at all. Where a machine's UART is, where its RAM begins and
which emulator boots it are not C library facts:

```toml
[dependencies]
riscv-virt-rt = { version = "0.5.0", features = ["nolibc"] }
```

⚠️ `std-freestanding-nolibc` is what that feature resolves to, and adding it
**directly** alongside a C library fails silently rather than loudly. A C library
ships as an archive, and
an archive member is pulled only while the symbol is still undefined; a
dependency package's object files enter the link unconditionally. The package
therefore defines `memcpy` first, the C library's member is never pulled, and
the build succeeds — with the byte-at-a-time implementations in place of the C
library's optimised ones, and no report of the substitution. Measured with
picolibc present: a cold build links, and `nm` finds one definition.

### Running tests on the target

`mcpp test` builds one image per `tests/*.cpp`, runs each under the emulator
the board-support package supplies, and reads the exit code as the verdict.
Semihosting propagates the firmware's `main` return value to the emulator's
exit code, so the model is identical to a hosted test run.

```bash
mcpp test
```

```
   Compiling boots (test)
     Running bin/boots
boots: console
boots ... ok (0.02s)

 test result ok. 1 passed; 0 failed; finished in 0.58s (build 0.05s + run 0.02s)
```

A failing case is named, and the command's exit status is non-zero:

```
deliberate_fail ... FAIL (exit 1, 0.02s)
about to fail
boots ... ok (0.02s)
boots: console

error: test result: FAILED. 1 passed; 1 failed; finished in 0.24s (build 0.04s + run 0.02s)

failures:
    deliberate_fail
```

### The artifact set for flashing

A freestanding link produces three files rather than one:

```
target/riscv64-none-elf/<fingerprint>/bin/blinky        91640 bytes   ELF, for a debugger or `qemu -kernel`
target/riscv64-none-elf/<fingerprint>/bin/blinky.bin     8664 bytes   flat image, what a flasher accepts
target/riscv64-none-elf/<fingerprint>/bin/blinky.map    253369 bytes  link map
```

The flat image is produced by an `objcopy -O binary` edge derived from the same
payload as the driver. The map is an implicit output of the link edge rather
than a bare `-Wl,-Map=` flag, so deleting it causes it to be regenerated; it is
the only artifact that answers why a section is where it is, and why something
was or was not pulled in from an archive.

### Overriding the runner a board-support package supplies

A board-support package normally supplies the runner. A project may override
it, which is the ordinary precedence — what the author of the project wrote
beats what a dependency supplied:

```toml
[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-no-reboot", "-bios", "default",
          "-s", "-S",                    # wait for a debugger on the first instruction
          "-kernel"]
```

The override is reported rather than applied silently:

```
        note [target.riscv64-none-elf].runner overrides the runner a dependency supplied
```

The artifact path is appended to the template, or substituted for `{}` when the
template contains that token. Appending is the common shape, because `-kernel
<image>` ends the line.

mcpp ships no default runner. Which emulator, which machine model and which
firmware mode are board facts — two boards on the same ISA need different argv,
`-bios default` for an OpenSBI boot against `-bios none -semihosting` for a
picolibc image — so an engine that guessed one would have to be fought by the
other.

## Diagnostics

### `import std` on a freestanding target

```
error: `import std;` is not available on 'riscv64-none-elf' — a freestanding target has no hosted standard library.
       `std` is one module over the entire library (threads, filesystem, iostreams
       included), so there is no subset of it to build without an OS underneath.
       Use the freestanding subset instead — an ordinary dependency carrying
       the parts of the library that need no OS (array, span, optional, atomic,
       string_view, ranges, expected, charconv, coroutines):

           [dependencies]
           std-freestanding = "0.2.0"

       then `import mcpplibs.std.freestanding;` in place of `import std;`.
       The target's C library itself comes from the BOARD package (riscv-virt-rt
       exports `mcpplibs.riscv_virt_rt`).
```

The message is emitted at configure time. Reporting a missing `std` module
source instead would send the reader to look for a broken payload, when nothing
is missing from the toolchain.

### An absent runner

A freestanding artifact cannot execute on the build machine: it has the wrong
ISA, no loader, and expects to own the address space. When no runner is
configured, `mcpp run` reports the gap between a successful build and an
impossible execution:

```
error: no runner is configured for 'riscv64-none-elf' — a freestanding artifact cannot execute on this machine.
       Declare how to run it:

           [target.riscv64-none-elf]
           runner = ["qemu-system-riscv64", "-machine", "virt",
                     "-nographic", "-no-reboot", "-bios", "default", "-kernel"]

       The artifact path is appended, or substituted for `{}` if the template contains it.
       A board-support package normally supplies this so you do not have to.
```

The key is not specific to bare metal. A hosted cross target — an
`aarch64-linux-musl` artifact on an x86_64 host — takes the same
`[target.<triple>].runner`, with a user-mode emulator such as
`qemu-aarch64-static` in place of the system emulator; on such a target an
absent runner is not an error until the kernel refuses the artifact. The rules
for hosted targets, the `--no-runner` escape and the not-run reporting of
`mcpp test` are in [5 — mcpp.toml](05-mcpp-toml.md), §2.7.3.

## Writing a board-support package

A board-support package is an ordinary mcpp package. It declares the emulator
it needs under `[xlings.workspace]`, exports one C++ module for consumers, and
emits its board facts from `build.mcpp`.

**A declaration there provisions the package on the first build** (since
2026.8.29; the table is `[xlings.workspace]` since 2026.9.3.1, and the older
`[xlings] deps` still works and says so). It is also what lets `mcpp::xpkg_dir` answer *"where
did that package land"*. Both halves matter: the same declaration installs the
emulator and tells the build program where it went.

The provisioning is the contract `[toolchain]` has always had — declare it,
mcpp installs it on first use — and it obeys the same two knobs: under
`--offline` / `MCPP_OFFLINE` or `MCPP_NO_AUTO_INSTALL` mcpp refuses instead,
naming the packages so they can be installed out of band.

⚠️ **A build program still must not assume the directory exists.** Provisioning
runs for the package that DECLARES the deps; a build program can be reached
through paths where that has not happened — a dependency of a project that
declares nothing, an environment where the knobs above refused — so
`xpkg_dir` may still come back empty and the program must say so rather than
emitting a broken runner:

```cpp
if (const char* dir = mcpp::xpkg_dir("xim", "qemu-riscv"); dir && *dir) {
    mcpp::runner(std::format("{}/bin/qemu-system-riscv64", dir).c_str());
    // … the rest of the argv …
} else {
    mcpp::warning("qemu-riscv is not installed, so `mcpp run` has no runner. "
                  "Install it once:  xlings install qemu-riscv -y");
}
```

Without that line the build succeeds, configures no runner, and `mcpp run`
reports a missing runner with advice about writing a `runner` key — true in
general, and not the cause here. See `mcpp:warning=` in
[07 — build.mcpp](07-build-mcpp.md).

### The directives a board-support package emits

| Directive | Effect |
|---|---|
| `mcpp:link-lib=<name>` | Adds `-l<name>` to the consumer's link line. Bare names suffice: the target sysroot's library directory is already on the search path. |
| `mcpp:link-search=<dir>` | Adds `-L<dir>`, for a library the package carries itself. |
| `mcpp:link-script=<abs path>` | Adds `-T <path>`. A relative path resolves against the package root, so a script belonging to the target's C library must be named absolutely. |
| `mcpp:runner=<token>` | Appends one argv token to the run template. argv is ordered, so the template is built by repetition — one directive line per token. |
| `mcpp:include-dir=<dir>` | Adds an include directory **for this package only**. |

Two engine queries supply the paths a board-support package must not hardcode:
`mcpp::sysroot_dir()` returns the target's C library root, and
`mcpp::xpkg_dir(ns, name)` returns an installed payload's directory.
`mcpp::target_arch()` reports the architecture being built for.

### Directive scope and what reaches a consumer

The scopes are deliberately asymmetric, and the asymmetry is what allows the
engine to work without a sysroot concept in the directive layer at all:

| Scope | Directives | Reaches the consumer |
|---|---|---|
| `LinkGlobal` | `link-lib`, `link-search`, `link-script` | Yes |
| `RunGlobal` | `runner` | Yes |
| `PackagePrivate` | `include-dir`, `include-dir-after` | No |

A board-support package therefore includes the target's C headers privately and
exports what it wants visible as a C++ module. Consumers import that module;
they do not inherit an include path.

### A complete build.mcpp

The board-support package for QEMU's RISC-V `virt` machine is reproduced below
with its comments removed. It is the whole of the board's build logic:

```cpp
import mcpp;
import std;

int main() {
    const std::string arch = mcpp::target_arch() ? mcpp::target_arch() : "";
    const bool rv32 = (arch == "riscv32");

    // Selected out of the target's C library, by bare name.
    mcpp::link_lib("crt0-semihost");
    mcpp::link_lib("c");
    mcpp::link_lib("semihost");
    mcpp::link_lib(std::format("clang_rt.builtins-{}",
                               rv32 ? "riscv32" : "riscv64").c_str());

    // This machine's memory layout, asked for rather than declared.
    if (const char* sysroot = mcpp::sysroot_dir(); sysroot && *sysroot)
        mcpp::link_script(std::format("{}/lib/{}/picolibcpp.ld", sysroot,
                                      rv32 ? "rv32imac/ilp32" : "rv64gc/lp64d").c_str());

    // How to run an image. The emulator is named by absolute path, because a
    // bare name resolves through PATH to a shim that dispatches against its
    // own owner home rather than the home this build uses.
    if (const char* qemu = mcpp::xpkg_dir("xim", "qemu-riscv"); qemu && *qemu) {
        mcpp::runner(std::format("{}/bin/qemu-system-{}", qemu,
                                 rv32 ? "riscv32" : "riscv64").c_str());
        for (auto a : {"-machine", "virt", "-nographic", "-no-reboot",
                       "-semihosting", "-bios", "none", "-kernel"})
            mcpp::runner(a);
    }

    mcpp::rerun_if_env_changed("MCPP_TARGET_ARCH");
    return 0;
}
```

The package's manifest declares the emulator and nothing else:

```toml
[xlings.workspace]
qemu-riscv = "xim:9.2.4-1"
```

Linking `clang_rt.builtins` is not optional on this board. picolibc formats
floating-point values through ryu, which performs 128-bit shifts, and rv64 has
no instruction for them; without the builtins the link fails on `__ashlti3` and
`__lshrti3`. A check for 64-bit division does not reach this case, because
rv64gc has a hardware `divu`.

An mcpp older than the one a package was written for cannot be detected from
within the package: `if constexpr (requires { mcpp::runner("x"); })` is a hard
error on an unknown qualified name rather than `false`, so the language offers
no feature test here. The engine compensates by appending an upgrade note when
a `build.mcpp` fails to compile with an error naming a non-member of `mcpp`.

## Verification scope

The commands and outputs in this document were measured on 2026-08-20 with:

| Component | Version |
|---|---|
| mcpp | 2026.8.20.1, built from this repository |
| Host | `x86_64-linux-gnu` |
| Toolchain | `xim:llvm` 22.1.8 |
| Target C library | `xim:picolibc-riscv` 1.8.12 |
| Emulator | `xim:qemu-riscv` 9.2.4-1 |
| Board-support package | `mcpplibs:riscv-virt-rt` 0.3.0 |
| Standard-library subset | `mcpplibs:std-freestanding` 0.2.0 |

Sizes vary between mcpp versions: the same project measured `text 8844` under
2026.8.19.4 and `text 8572` under 2026.8.20.1. The numbers above are therefore
illustrative of magnitude rather than fixed values.

The bare-metal chain has continuous verification on Linux only. Engine-side CI
runs a `baremetal` job covering four end-to-end scripts, and the two ecosystem
packages run RISC-V 64 and 32 under QEMU in their own repositories — all on
`ubuntu-24.04`. macOS and Windows hosts are expected to work, because the
payload is a cross-compiler and `xim:qemu-riscv` publishes assets for five host
targets, but that expectation is **not** covered by a test.

## Current limitations

| Limitation | Observed behaviour |
|---|---|
| `std::format`, `std::sort` over builtin scalar types, and a complete `std::string` | Fail at **link** time naming the undefined symbol. libc++ places these entities in the compiled library — the scalar `__sort` instantiations are `extern template`, with no macro that disables them — so a target-built `libc++.a` is required. No such payload is published. |
| Exceptions and RTTI | Disabled across the whole graph. `try`/`catch` is unavailable at compile time. A board shipping a target-built `libc++abi` and unwinder has a genuine case for re-enabling them; that is the point at which this becomes a manifest key. |
| Board coverage | One board family. `riscv32-none-elf` demonstrates that the ISA table is data, not that a second machine has been ported. ARM Cortex-M has not been attempted. |
| C library substitution | Expressible since 2026.8.20.2 through `[target.<triple>].sysroot`, and **verified only for the empty value** (the zero-libc tier). Pointing it at a different C library is accepted and installed through the same channel, but no second bare-metal C library is published, so that path is untested. |
| `qemu-riscv` on `win32-arm64` | The upstream package publishes no asset for that host, so installation fails on it. The failure is correct rather than silent, but the host cannot run a bare-metal image. |
| Ecosystem CI breadth | The two ecosystem packages run their own CI on `ubuntu-24.04` only. mcpp-index's `tests/examples/` workspace members run unconditionally on three platforms with no capability gate, so a package requiring an emulator and a target sysroot cannot be added there. This is a known coverage gap. |
