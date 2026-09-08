# 22 — The Target Side

**Reader:** an author whose one manifest must serve several targets.

**The question this chapter answers:** how does a manifest say "only there", and
what may be conditioned that way.

**Not here:** the vocabulary of target names, which is
[21 — The Target Triple](21-the-target-triple.md), and the accelerator axis,
which resolves after the graph and is
[42 — Heterogeneous Builds](42-heterogeneous-builds.md).

A build must answer one question before it can emit a command line: where the
target's compiler runtime, platform interface, C library and C++ runtime come
from. mcpp resolves that question once, after the dependency graph is known,
and every later stage reads the single resolved value.

This document specifies the model, the rules that govern it, what a project
writes, and what a package declares.

## The Five Layers

The target side of a build consists of five layers.

| Layer | Content | Implementations |
|---|---|---|
| `compiler` | the program that compiles | `llvm`, `gcc`, `msvc` |
| `compiler-runtime` | the compiler's own runtime: integer and floating-point builtins, the unwinder | `compiler-rt` with `libunwind`, `libgcc` |
| `kernel-abi` | the platform interface, or its equivalent | `linux`, `windows`, `darwin`, `openkal` |
| `c-abi` | the C library | `glibc`, `musl`, `picolibc`, `ucrt`, `libSystem` |
| `c++-abi` | the C++ library and its ABI runtime | `libc++` with `libc++abi`, `libstdc++`, MSVC STL |

### Membership Criteria

A component is a layer when three conditions hold simultaneously: at least two
interchangeable implementations exist; it can be replaced independently of its
neighbours; and it stands in a definite "was configured for" relation to the
layer beneath it. A component failing any one of the three belongs to an
adjacent layer rather than to one of its own.

`compiler-runtime` is separate from `c++-abi` because the builtins are what a
C program requires. Treating them as part of the C++ runtime asserts that a C
program needs no integer division, and that assertion has already produced a
measured defect: a C program cross-compiled to macOS was asked whether a C++
runtime was present, answered that none was, and the link line consequently
retained the compiler payload's own `libc++`.

`kernel-abi` is unnamed on a conventional stack, where a C library issues
system calls or invokes platform entry points directly. Naming the seam is what
permits one C library implementation to sit above several platforms.

## The Four Origins

Each layer is supplied from one of four origins.

| Origin | Meaning | Known |
|---|---|---|
| `payload` | the compiler payload carries it | before dependency resolution |
| `prebuilt` | a named prebuilt payload supplies it | before dependency resolution |
| `graph` | a package in the dependency graph supplies it | after dependency resolution |
| `—` | nothing supplies it, and that is a statement | — |

Two origins are knowable before dependency resolution and two only after it.
The target side is therefore resolved exactly once, at the point where the
graph exists. Any earlier derivation is an inference about a fact that does not
yet exist, and independent inferences about such a fact do not agree.

An absent layer is an answer rather than a gap. A bare-metal target has no
kernel; a project that depends on no C library has no C library.

## The Rules

### One Supplier Per Layer

A C library, a platform interface and a C++ runtime are mutually exclusive
choices rather than additive contributions. Two suppliers for one layer is an
error, reported during resolution and naming both packages together with how
each entered the graph.

The failure mode motivates the strictness: selecting the wrong supplier does
not fail the link. It produces a program that runs and intermittently does not.

### Configured For The Layer Beneath

An implementation is usable only above the layer it was configured for. A
`libc++` build records that configuration in its `__config_site`; a `libgcc`
build is configured for GCC. The relation is declared rather than inferred —
see [`requires`](#requires) below.

Two consequences follow. The compiler payload's C++ runtime is eligible only
when the C library is also the payload's. A compiler runtime must belong to the
compiler's own family, since a build in which the two disagree resolves
`__udivti3` differently from every other link in the same program.

### Cross-Origin Wiring

The engine wires two layers together only when they come from different
origins.

| Combination | Relation expressed by | Engine |
|---|---|---|
| both from `graph` | ordinary dependencies between packages | no involvement |
| both from `payload` | the payload is internally consistent | no involvement |
| one prebuilt, one from the graph | only the engine knows both addresses | wiring required |

Moving a layer from a prebuilt payload into the dependency graph therefore
removes engine work rather than adding it. This is the mechanism by which one
source reaches several platforms without an engine change.

### Layer Names Are Fixed, Implementations Are Not

The five layer names are a closed set compiled into the engine. The
implementations filling them appear in package manifests and in the index, and
in no line of engine code.

Layer names may be fixed because the layers are determined by the C and C++
build model and do not grow. Implementations may not, because growth is
precisely what they do: an ecosystem's combinations are the product of its
implementations while its packages are their sum.

## What A Project Writes

Layer names do not appear in a project manifest. A project expresses its target
side through three existing mechanisms.

### The Target Triple

`--target <triple>`, or `[build] target`. The OS field selects the platform
interface. The environment field states a request for a C library; it is a
request rather than the answer, and the resolved value is reported by the
build.

Omitting the field declines to state one: `x86_64-linux` asks for whatever
supplies that layer, and `x86_64-linux-musl` asks for musl. When the dependency
graph supplies a different one the graph decides, and the build reports that the
name is inaccurate together with the spelling to use instead. The request is
ignored rather than violated, so the artifact is the same either way.

### The Toolchain

`mcpp toolchain default <family>@<version>`, `[toolchain]` in the manifest, or
`[target.<triple>].toolchain` for one target. This selects the `compiler`
layer, which is the one layer no package may supply.

A target row may carry a convention — a toolchain whose payload supplies that
target's C library. The convention applies when the manifest states nothing for
that target AND nothing in the dependency graph supplies the target's system.
The second condition is knowable only after resolution, so the toolchain is
resolved there rather than before it.

### Dependencies

Every other layer is selected by depending on a package that supplies it. A
single dependency may supply several layers, and may bring further suppliers
through its own dependencies.

```toml
[dependencies]
openkal-llvm-runtime = "0.1"
```

## What The Build Reports

The build prints what it resolved. A manifest line states an intention that
goes stale when the packages beneath it change; a report states the outcome and
cannot.

By default the report lists only the layers the compiler payload did not
supply. A zero-configuration build resolves all five from one payload, and five
lines reading `(payload)` answer a question nobody asked.

```
      Target x86_64-linux-gnu
```

```
      Target x86_64-windows-gnu → x86_64-w64-windows-gnu
             kernel-abi        openkal        (openkal-windows@0.1.3, graph)
             c-abi             musl           (openkal-musl@0.3.3, graph)
             c++-abi           libc++         (openkal-llvm-runtime@0.1.1, graph)
```

`MCPP_VERBOSE=1` lists all five. Diagnostics always list every layer the
decision rested on, including the ordinary ones, because an error that omits
its evidence cannot be checked by its reader.

Interface and implementation are separate columns. `openkal` is an interface
and `openkal-windows` an implementation of it; collapsing the two would conceal
why one source reaches several machines.

## What A Package Declares

### provides

A package supplying a layer declares it in the reserved `mcpp:` namespace.

```toml
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
```

The grammar is `mcpp:<layer>[=<implementation>]`. The layer name is validated
against the closed set; a misspelling is an error rather than a silently
disabled behaviour. Names outside the prefix belong to the feature system and
pass through unexamined.

`mcpp:compiler` may be required but not provided. A compiler is a payload this
engine installs and drives, and the differences between families — flag
spellings, the module model, the BMI format, the driver configuration file —
are facts the engine must hold rather than data a package can describe.

### requires

`requires` is the symmetric half, and the mechanism by which the layering rule
is enforced without an implementation name in the engine.

```toml
requires = ["mcpp:compiler=llvm"]
```

A C++ runtime built from `libc++` sources is compiled, and its module compiled,
by Clang. That fact belongs to the package. The engine checks a relation it can
state generically — the named layer must resolve to the named implementation —
and reports a mismatch by naming both, which a table of families compiled into
the engine could not do for a family it had never heard of.

The check runs before compilation begins. The combination it rejects otherwise
fails inside the runtime's own headers, in a message naming a file the reader
has never opened and no decision mcpp made.

### Standard Library Module Sources

A package that is a standard library states where its `std` module source is
and what that source requires.

```toml
[build]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc", "-nostdinc++"]
```

These keys belong to `[build]` because the module source is one of the
package's translation units: it is compiled with the package's include
directories and definitions. Membership of `[build]` also makes the flags
conditional, which a package supplying one C++ runtime over several C libraries
requires.

```toml
[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]
```

Declaring `std-module` without a matching `provides` entry is an error: the
package describes a library it does not supply.

The `[package]` spelling of these three keys remains accepted and is not
conditional.

### Adaptation To The Resolved Target Side

A package supplying a layer frequently supports several implementations of the
layer beneath it. It queries the resolved target side rather than being told.

```toml
[target.'cfg(c-abi = "musl")'.build]
include_dirs = ["config/musl"]

[target.'cfg(c-abi = "picolibc")'.build]
include_dirs = ["config/picolibc"]
```

Requiring a feature selection for this would oblige a project to restate what
the target triple or its dependency graph has already established, and permit
the two statements to disagree.

The predicate keys are the five layer names, and their values are the interface
names in the table at the top of this chapter — the same strings the `Target`
report prints. They combine with the triple keys under `all`/`any`/`not`:

```toml
[target.'cfg(all(linux, c-abi = "musl"))'.build]
cxxflags = ["-D_GNU_SOURCE"]
```

**A layer names the library, not the triple's env segment.** They coincide
for `musl` and diverge for `gnu`: on Linux that segment asks for glibc, and on
Windows it names the MinGW flavour of the toolchain, whose C runtime is the same
UCRT the MSVC flavour links. The spelling is `c-abi = "glibc"`, never
`c-abi = "gnu"`; the request, as opposed to the answer, is `env = "gnu"` — a
different question (`docs/specs/target-side.md` §3.4).

**`env` and `c-abi` are not interchangeable.** `env` is what the triple
*asked* for; `c-abi` is what the graph and the payload *answered*. An
`openkal-musl` in the dependency graph supplies musl under an `x86_64-linux-gnu`
triple, and only `c-abi` sees that.

These predicates are available in `[build]` sections only. The target side is
resolved after dependency resolution, so a dependency selected by one would
form a cycle; `[target.'cfg(<layer> = …)'.dependencies]` is reported and
ignored rather than silently dropped. A package whose C libraries require
different dependencies is split per C library, or depends on the union and
selects sources in `[build]`.

A key mcpp does not know — a typo, or a predicate from a newer mcpp — is
reported as a schema warning and the section does not apply. It used to
evaluate to false in silence, which reads exactly like a section that correctly
did not match.

## Diagnostics

Four conditions are reported by the engine rather than by a compiler.

| Condition | Report |
|---|---|
| a required implementation is not what resolved | names both, and the command that selects it |
| two packages supply one layer | names both, and how each entered the graph |
| a layer has no supplier | names the layer, and the capability to depend on |
| the payload's C++ runtime sits above a foreign C library | names both, and the two ways out |

A message from a compiler or a linker about a target-side combination
indicates a missing diagnostic. The engine knows the combination is untenable
before any command line is emitted.

## Compatibility

Three provisions preserve existing manifests and existing builds.

The capability `hosted-standard-library` continues to denote the C++ layer. A
package carrying both spellings is one supplier, and the entry naming an
interface is the one reported.

The toolchain family spelling `openkal-llvm` normalises to `llvm`. It named the
same payload and carried a fact about the target side, which the model above
resolves from what packages declare.

An unknown name inside the reserved prefix is an error in the root project's own
manifest and a warning in a dependency's. The first is a misspelling the author
is looking at; the second is a manifest written against a newer engine, and
refusing it would mean the layer vocabulary could never be extended by a
published package. An unknown key elsewhere in a manifest is ignored.

That provision governs future engines only. A package declaring a layer name
still requires its consumers to run an engine no older than the release that
introduced the name.

## `[target.*]` — Platform-Conditional Dependencies & Flags

Scope dependencies and build flags to a platform with a `[target.<sel>]` table.
The selector `<sel>` has three forms:

| Selector | Meaning | Example |
|---|---|---|
| **bare OS alias** | a single OS / family — the concise, common form | `[target.windows]`, `[target.unix]` |
| **`cfg(...)` predicate** | a compound condition (arch / env / combinators) | `[target.'cfg(all(linux, not(arch = "aarch64")))']` |
| **exact triple** | one specific target (also carries `toolchain` / `linkage` / `sysroot` / `runner`; see [04 §2.7.3](04-mcpp-toml.md)) | `[target.x86_64-linux-musl]` |

A selector may carry platform-conditional **dependencies** and **build flags**:

```toml
# Concise bare-alias form — pull OpenBLAS and link it only on Windows.
[target.windows.dependencies.compat]
openblas = "0.3.33"
[target.windows.build]
ldflags = ["-Llib", "-llibopenblas"]

# cfg(...) for compound predicates (grammar: all/any/not over os/arch/family/env,
# plus the bare aliases windows/unix/linux/macos).
[target.'cfg(all(linux, not(arch = "aarch64")))'.build]
cxxflags = ["-march=x86-64-v2"]
```

`[target.windows]` is exactly equivalent to `[target.'cfg(windows)']` — the bare
aliases `windows` / `linux` / `macos` / `unix` are never valid target triples, so
there is no ambiguity. Use the bare form for a single OS/family; use `cfg(...)`
for arch/env conditions and combinators.

- **Keys**: `dependencies` / `dev-dependencies` / `build-dependencies` /
  `feature-deps.<feature>` (mcpp 2026.8.6.2+ — see [30 — build.mcpp](30-build-mcpp.md); the feature is
  registered unconditionally, only its dependency set is scoped), and
  `build` with `cflags` / `cxxflags` / `ldflags` / `sources` (mcpp 0.0.95+ —
  conditional source globs, e.g. gating `src/x86/**/*.asm` behind
  `cfg(arch = "x86_64")`; `!`-exclusion globs work here too), plus `flags` and
  `include_dirs` / `include_dirs_after` (mcpp 0.0.102+), plus
  `private_include_dirs` and `std-module-flags` (mcpp 2026.9.1.1+).
- **What `build` accepts is exactly the set of *additive build inputs*** — the
  things that combine by appending and are consumed after the predicate is
  evaluated, which is the member list of `BuildInputs`. `linkage`, `target`,
  and the profile knobs are deliberately not among them: they are *inputs to*
  target selection (conditioning `target` on a predicate evaluated against
  `target` is circular), or they need override-rather-than-append semantics.
  A key outside the set is reported and ignored; the message lists the set it
  checked against, so it cannot drift from the check.
- **Evaluated against the resolved target** — the `--target` triple for a cross
  build, otherwise the host. So a native Linux build never even *downloads* a
  `[target.windows]` dependency.
- **Predicate keys**: `os`, `arch`, `family`, `env` — the triple's coordinates —
  and, from mcpp 2026.9.1.1, the five target-side layer names `compiler`,
  `compiler-runtime`, `kernel-abi`, `c-abi`, `c++-abi`
  ([22 — The Target Side](22-target-side.md)). `accelerator` is a key here too
  and is answered from this build's own `accel` — the backend names in
  `--accel` or `[build] accel` — so it is a membership test over a set, and
  `accelerator = "none"` is how a section says "this build named no backend"
  without enumerating the ones it is not. Barewords `linux` / `macos` /
  `windows` / `unix` are sugar for the matching `os` / `family` test. A key
  outside this set is reported as a schema warning and the section does not
  apply — it used to answer false in silence, which is indistinguishable from
  a section that correctly did not match.
- **A resolved-layer predicate cannot select dependencies.** A layer is
  resolved *from* the dependency graph, so a dependency chosen by one would
  decide the answer it is asking for. `[target.'cfg(c-abi = "musl")'.dependencies]`
  is reported and ignored; the `build` inputs under the same predicate do apply.
  `accelerator` is not one of these (mcpp 2026.9.6.5): it is an input to the
  build rather than an answer from the graph, so
  `[target.'cfg(accelerator = "cuda")'.dependencies]` applies.
- **Precedence**: an exact-triple table wins over a `cfg`/alias table; multiple
  matching predicate tables have their flags concatenated. Conditional entries
  are appended **after** the unconditional `[build]` ones, so under GNU
  "last flag wins" a conditional rule overrides a broader unconditional one.
  That is what makes a per-OS **removal** expressible:

  ```toml
  [build]
  flags = [{ glob = "third_party/zlib/**", defines = ["HAVE_UNISTD_H=1"] }]

  # clang-MSVC has no <unistd.h>: undo the base define, add the windows one.
  [target.'cfg(windows)'.build]
  flags = [{ glob = "third_party/zlib/**",
             defines = ["NO_FSEEKO"], cflags = ["-UHAVE_UNISTD_H"] }]
  ```

- **A conditional `flags` entry that does not match the current target does not
  exist at all**, so it cannot produce a "glob matched no source file" warning.
  One manifest can therefore carry all three OSes' flag tables without any of
  them generating noise on the other two — the same way an inactive feature's
  entries simply are not there. A zero-hit glob in the *unconditional* table
  still warns, because there it is a real defect.
- **`toolchain` / `linkage` / `sysroot` are exact-triple only** — they describe
  one specific cross target, so put them under `[target.<triple>]` (above), not
  under a bare alias or `cfg(...)`.

### `sysroot` — the target's C library

`sysroot` (mcpp 2026.8.20.2+) overrides the C library the target table binds to
a triple, on the same axis as `toolchain` overriding the compiler pin: one names
the compiler a target resolves, the other names its C library, and both were
engine-only until a project had a reason to disagree.

```toml
[target.riscv64-none-elf]
sysroot = "xim:newlib-riscv@4.4"     # a different C library
```

```toml
[target.riscv64-none-elf]
sysroot = ""                          # no C library at all
```

**An absent key and an empty one are different answers.** Absent inherits the
target table's C library. Present-and-empty is the **zero-libc tier**: no C
library is resolved, no include or library path is added, and the link carries
only what the project and its dependencies supply. `#include <stdio.h>` stops
resolving. A kernel or a bootloader wants exactly that, and collapsing the two
cases would silently hand such a project the target's C library back.

The value is an xpkg reference or the empty string; a bare name is rejected when
the manifest is parsed, because accepting it would install nothing and then fail
much later naming a missing libc.

A build program can ask which C library **payload** supplies the sysroot:
`mcpp::target_libc()` returns that package's name and
`mcpp::target_libc_profile()` the sub-directory for the target's ISA profile.
Both are empty on the zero-libc tier. See
[40 — Bare-Metal and Freestanding Targets](40-baremetal.md).

**That is not the same question as "which C library did the target side
resolve to".** `target_libc()` names the payload mcpp installed, and that value
is an *input* to target-side resolution — a package in the dependency graph can
supply the C library instead, in which case the resolved `c-abi` is not what
this returns. To branch on the resolved layer, use a layer predicate:
`[target.'cfg(c-abi = "musl")'.build]` ([22 — The Target
Side](22-target-side.md)). This paragraph said "which C library was resolved"
until 2026.9.1.1, which was the wrong one of the two.

## Current limitations

- **A dependency cannot be conditioned on the accelerator.** The accelerator
  layer is resolved from the dependency graph, so a dependency chosen by it would
  decide the answer it is asking for. mcpp reports the predicate and ignores it;
  packages are unconditional or conditioned on the platform, and `[build]
  sources` is what the accelerator selects.
- A layer cannot select a dependency for the same reason, in the general case:
  any derivation earlier than the graph is an inference about a fact that does
  not yet exist.
