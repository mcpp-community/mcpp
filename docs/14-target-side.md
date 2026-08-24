# The Target Side

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
| `c-abi` | the C library | `glibc`, `musl`, `picolibc` |
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

### The Toolchain

`mcpp toolchain default <family>@<version>`, `[toolchain]` in the manifest, or
`[target.<triple>].toolchain` for one target. This selects the `compiler`
layer, which is the one layer no package may supply.

A target row may carry a convention — a toolchain whose payload supplies that
target's C library. The convention applies when the manifest states nothing for
that target. When it replaces a default set with `mcpp toolchain default`, the
status line reports the substitution and names the one-line override.

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

These predicates are available in `[build]` sections only. The target side is
resolved after dependency resolution, so a dependency selected by one would
form a cycle. A package whose C libraries require different dependencies is
split per C library, or depends on the union and selects sources in `[build]`.

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

An unknown name inside the reserved prefix is an error; an unknown key elsewhere
in a manifest is ignored. A published package therefore continues to load under
an engine predating a key it carries.
