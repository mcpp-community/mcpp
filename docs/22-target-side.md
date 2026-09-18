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

## Closure Visibility

The five-layer report above answers "where does each layer come from"; it
says nothing about the OTHER packages in the graph — a dependency bound to
one platform's SDK is exactly as invisible to it as an ordinary one. Two
mechanisms close that gap (design 2026-09-18 §6).

**What counts as a platform dependency, precisely.** A package brings one if
and only if it says so — `provides = ["platform-sdk"]`, an ordinary,
unprefixed capability. Nothing here is inferred from header paths, link
flags or a dependency's `visibility`: inference would have exactly the
silent-typo failure mode the reserved `mcpp:` prefix exists to avoid for the
five layers, applied to a fact this engine cannot otherwise observe. [06 —
A platform SDK dependency stays private](06-features-and-capabilities.md#a-platform-sdk-dependency-stays-private)
is the pattern such a package's own manifest follows.

**The report.** A build's `Target` report gains a line naming every package
in the graph that declares `platform-sdk`, empty when none:

```
      Target              platform-deps     —
```

```
      Target              platform-deps     some.windows-headers@1.0.0
```

Printed under the same visibility rule as the five layers: only when there
is something to report, or always under `MCPP_VERBOSE`.

**The refusal.** `[build] platform-dependencies = "refuse"` fails the build
outright when the graph contains one — the machine-checkable form of "this
build is a closure entirely on its kernel-abi implementation and nothing
else":

```toml
[build]
platform-dependencies = "refuse"
```

The only accepted value is `"refuse"`; absent (the default) allows platform
dependencies, today's behaviour.

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

The same statement is the recipe for a package whose install hook compiles a
static library from source. The library is compiled against one C++ standard
library and cannot be linked into a program that uses another, and the store
directory it is installed into is keyed by package and version, not by that
choice. Such a package declares the implementation it was built for:

```toml
requires = ["mcpp:c++-abi=libstdc++"]
```

A project whose toolchain resolves another `c++-abi` is then refused, naming
both implementations, instead of failing at the link. The check runs once the
toolchain is resolved, which is after the dependency graph is installed, so the
install hook has already run; the hook receives the build's target but no
toolchain values ([32 — Authoring a Payload](32-authoring-a-payload.md)). It
must not build a different variant into the same store directory, because the
first consumer would then decide the variant for every later one.

### The C Environment A `c-abi` Package Presents (mcpp 2026.9.18+)

A traditional stack never has to say this: the compiler payload's target
triple already implies the environment a C program compiles against. The
moment a package supplies the C library instead, that stops being true —
openkal-musl on `x86_64-windows-gnu` generates PE/Win64 code while
presenting a POSIX environment to source, because it is a musl port and
every `#ifdef _WIN32` in the libraries above it is asking the wrong layer.
The `[c-abi]` block is how the C library states, once, what it actually
presents — and only the package that supplies the layer may state it.

```toml
# openkal-musl's manifest
[package]
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"        # posix | windows | none
data-model = "arch-default"  # arch-default | lp64 | llp64 | ilp32
wchar      = 32               # 16 | 32
builtins   = "iso"            # iso | platform (default platform)
```

**Who may declare it.** A package that writes `[c-abi]` without also listing
`mcpp:c-abi=<impl>` in `provides` is stating a fact about a layer it does not
supply, and that is always wrong rather than merely unusual — it is refused
at manifest parse time, naming the missing `provides` entry.

**The four keys, and their closed value sets.**

| Key | Values | Answers |
|---|---|---|
| `presents` | `posix` / `windows` / `none` | which environment-identity macros source sees (`__unix__` vs `_WIN32` vs neither) |
| `data-model` | `arch-default` / `lp64` / `llp64` / `ilp32` | how wide `long` is |
| `wchar` | `16` / `32` | how wide `wchar_t` is |
| `builtins` | `iso` / `platform` (default) | whether the compiler may assume the platform C library's own extensions |

`presents`, `data-model` and `wchar` carry no default: a block that omits one
of them is refused naming the missing key, because "absent" is not the same
statement as any of the three closed values could make. `builtins` alone
defaults to `platform`, today's behaviour. An unrecognised key or an
unrecognised value is always a parse error naming the key — never silently
ignored. **A package that declares no `[c-abi]` block changes nothing**: the
resolved target side, every compile command and every cache key are
byte-identical to a build before this feature existed.

Three facts, kept separate, because none of them implies another: `presents`
picks the source branch, `data-model`/`wchar` pick the ABI. POSIX does not
imply LP64 (it is ILP32 on a 32-bit architecture), and LP64 does not imply
POSIX.

**Realisation.** Once the `c-abi` layer resolves to a package that declares
this block, mcpp turns the request into compiler configuration for every
target-side unit — the C library itself, the C++ runtime, the compiler
runtime's builtins, and every ordinary package in the graph — covering C,
C++ and assembly compiles, the dependency scan, and the `std` module
precompile alike. Assembly (`.S`/`.s`) needs its own broadcast channel to get
this: a `.S` unit's command line is assembled independently of a `.c`/`.cpp`
unit's (`mcpp.build.flags::CompileFlags::as`, not `::cc`/`::cxx`), and only
takes the `-D`/`-U`/`-I` words out of a package's C flags on purpose — a
`-std=` or `-O` token meant for the C compiler is meaningless to GAS — so the
realised environment tokens (`--target=`, `-f[no-]short-wchar`, and anything
`builtins = "iso"` adds) are broadcast a second time, verbatim, into that
narrower channel (found missing, and fixed, by the openkal-musl spike: a
`.c` unit in a package saw `_WIN32` undefined while a `.S` unit in the SAME
package — real code, like `okm_setjmp.S` and upstream libunwind's
`assembly.h`, selects register-save sets on it — still saw it defined). mcpp
holds one mapping table from request to triple and flags, generic knowledge
that names no C library:

| Target | Request | Realisation |
|---|---|---|
| Linux | `posix` / `arch-default` | the default triple already satisfies it |
| macOS | `posix` / `arch-default` | the default triple already satisfies it |
| Windows | `posix` / `arch-default` | Cygwin-flavoured: `--target=x86_64-pc-cygwin` on the compile line only; `__CYGWIN__`/`__CYGWIN32__` are left defined (see the note below); `data-model` becomes LP64 as a consequence of the triple, not a separate flag |
| any | `builtins = "iso"` | turns off code-generation idioms that assume a platform C library — `-fno-builtin-memset_pattern16` on Apple targets is the one this survey measured; see `src/toolchain/cenv.cppm` for what else was checked and found not to apply |
| anything else | | refused, naming the target, the request and what is missing — never a silent downgrade |

The Windows row is the flagship case: `x86_64-w64-windows-gnu` and
`x86_64-pc-cygwin` produce IDENTICAL machine code — same PE format, same
Win64 calling convention, same SEH — and differ only in what the
preprocessor sees and how wide `long` is. Realisation therefore touches only
the **compile** line; the **link** line keeps the triple the graph resolved,
because nothing about the object format changed.

**`__CYGWIN__`/`__CYGWIN32__` are left defined — a revision from the
openkal-musl spike, not the design's original claim.** Undefining them was
tried first, on the reasoning that a real Cygwin userland is not in the
graph. Portable third-party code that needs to know the **object format** —
not the C environment, not the platform API — has no name for "PE format
with a POSIX-presenting C environment" other than `__CYGWIN__`, and such code
cannot be patched the way this ecosystem's own packages can. `presents =
"posix"` answers one question, which environment-identity macros source
sees; it does not get to answer a different one, what object format this is,
by deleting the only macro that names it. This is a **trade-off for the
30-member measurement to settle, not a settled fact**: a library reaching for
`__CYGWIN__` may also reach for a real Cygwin interface (`sys/cygwin.h`,
`cygwin_conv_path`) that does not exist here, and if defining it produces
more new failures than it fixes, the answer flips.

**A `kernel-abi` provider's own units are INFERRED onto the platform boundary
— it never has to say so (mcpp 2026.9.18+, a mid-PR revision from the
openkal-musl spike).** A package that provides `mcpp:kernel-abi=<impl>`
(openkal-windows, say) has to see the platform's own environment — it
includes platform declarations and `_WIN32` must be true for it — and it
always will, by definition: such a package's whole job is to speak the
platform's ABI, so it can never be the package that wants the graph's
*presented* `[c-abi]` environment instead of the triple's own. mcpp does not
wait to be told this. Any package whose `provides` names
`mcpp:kernel-abi=<impl>` gets `c-environment = "platform"` as its default,
with no key of its own:

```toml
[package]
provides = ["mcpp:kernel-abi=openkal"]
# no [package] c-environment line — the boundary is inferred from `provides`
```

**Why inference and not just the flag.** The flag alone works; what it
cannot do is retroactively fix a package that has already shipped without
it. openkal-windows 0.8.0, openkal-macos 0.10.0, openkal-linux 0.13.0, and
every future kernel-abi implementation, get the boundary right — with no new
release and no coordinated version bump across repositories — because
`provides = ["mcpp:kernel-abi=<impl>"]` is the one fact they already state.
The failure this closes was measured, not hypothetical: openkal-windows,
compiled under the POSIX substitution like everything else in its graph,
got a 32-bit `wchar_t` from `-fno-short-wchar` while the Win32 calls it
makes hand back genuine 16-bit UTF-16 — a `wchar_t*` loop then read two
UTF-16 units as one code point. Making the boundary a default rather than a
manifest key a package must remember turns that failure class
unrepresentable rather than merely documented.

**Precedence: an explicit `c-environment` in the package's own manifest
always wins over the inference.** The inference only fills `cEnvironment`
when the package wrote nothing — a package that, after all, needs the
presented environment can still say so explicitly (there is no way today to
write "not platform" back, because `"platform"` remains the only value this
key accepts). The explicit key also stays the ONLY mechanism for §5.3's
other category — an ordinary package that is not the kernel-abi boundary but
still has platform-bound units of its own — where the author really is
making a choice mcpp cannot infer:

```toml
[package]
# an ORDINARY package, not a kernel-abi provider — the engine cannot infer
# this one; the author states it because platform-bound units are a real
# minority of what this package builds (design §5.3)
c-environment = "platform"
```

This is a boundary rule, documented rather than enforced by the engine
beyond the flag itself: the interface such a package exposes to the rest of
the graph must still cross in fixed-width types only (SPEC §5.4).

**Verification, not trust.** A declaration is checked, never trusted — the
same rule openkal applies to its own conformance claims. Once the tokens
above are known, mcpp compiles one syntax-only probe (`-E -dM`, a predefined-
macro dump — cheap, and it needs no execution, which matters because the
realised environment is routinely a cross target) with them and reads back
`__SIZEOF_LONG__`, `__SIZEOF_WCHAR_T__` and which environment-identity
macros are defined, comparing them against the declaration. A mismatch fails
the build and prints both the declared and the measured values. The result
is cached per configuration (compiler binary identity + exact flags), so a
build that resolves the same configuration twice pays for the probe once.

**Fingerprint.** The realised environment participates in the build's
fingerprint (`compileFlags`, §92's field 7): two builds whose C library
declares `lp64` and `llp64` compile the same source into objects whose
`long` disagrees in width, so they never share an output directory, and
neither can reuse a cached object the other produced.

**The global build cache's key also covers it (mcpp 2026.9.18+, a mid-PR
fix, not the design's original text).** `~/.mcpp/build-cache/v1` — the
cache an ordinary dependency compile reuses across projects and across an
`mcpp` upgrade — is a SEPARATE mechanism from the build fingerprint above,
keyed per package from exactly the axes that reach that package's own
compile command line (`mcpp.build.cache_key`). The realised environment
reaches a package's command line entirely through an engine BROADCAST (the
same channel `targetSideUsage` and `-D__openkal__` use, never the package's
own declared `[build] cflags`/`cxxflags`), so the key's own derivation had
to be told to read the broadcast, not only the declaration — found exactly
that way (coordinator report, openkal-musl spike): upgrading `mcpp` in
place, with the cache directory left in place, served objects compiled
under the OLD realised environment into an image built under the new one,
two C environments in one image, with no diagnosis at all. `fill_package_
config` now folds in `PackageRoot::privateBuild.cflags`/`cxxflags`/
`asmflags` — the post-broadcast values — alongside the package's own
declared flags, exactly as it already did for include directories.
`--cache=off`, or clearing the cache directory, was never a sign the key
was RIGHT; both routes bypass it entirely.

**This release also bumps the cache's epoch, orphaning every existing
entry — the first build after upgrading is cold.** A corrected key does not
by itself make an entry written under the old, wrong derivation safe to
keep: an entry is poisoned exactly when its recorded key and its actual
compiled inputs already disagreed, and the package MOST likely to still
show an unchanged key after the fix is the one this same revision newly
exempts from the realisation (a `kernel-abi` provider inferred into
`c-environment = "platform"`, above) — its `privateBuild.cflags` is now
empty, so the new key is computed from nothing, matching the OLD key, which
was also computed from nothing, while the object on disk was compiled WITH
the substitution. No cheaper check tells a pre-fix entry from a post-fix
one, so `mcpp.build.cache_key::kCacheEpoch` moves (2 → 3), which orphans
the whole cache unconditionally rather than trust a key equality that
cannot be trusted for exactly the entries that matter most.

**Store key — not yet closed, and here is exactly what that means (checked
against the code path that populates it, mcpp 2026.9.18+).** A package's
*install hook* CAN and does compile target-side code — object code, a
static library — and the shared store it installs into is keyed by package
and version only, the same gap [requires](#requires) already documents for
a C++ runtime selection. What makes this different from the build-cache key
above, and NOT something this PR could close the same way: an install hook
runs BEFORE the toolchain resolves, by a real ordering constraint, not an
oversight. `install_hook_env`'s toolchain fields are unconditionally empty
on the ordinary path — `prepare.cppm` resolves `tc` only *after* the
dependency graph installs, because resolving the target side can itself
depend on which package the graph turns out to supply a layer from (the
`c-abi` provider is a member of that same graph). A hook has no realised
environment to consult because, at the moment it runs, none has been
computed yet — there is nothing to pass it, not merely something mcpp
forgot to pass.

**The failure mode, plainly, not as a line in a gap table:** a hook that
compiles environment-sensitive C code (anything whose correctness depends
on `wchar_t` width, the data model, or which environment-identity macros
are defined) has no way to ask what this build realised, so it can only
compile against ONE assumption and hope every consumer shares it. A project
whose graph declares a `[c-abi]` that disagrees produces objects sized for
one `wchar_t` linked against headers sized for another, with **nothing
checking it** — the store records no environment for what it holds, so
there is no mismatch to detect, only a silently wrong link. This is the
identical shape the C++-runtime `requires` check above already accepts as a
documented limit, not a new one this PR introduces; `[c-abi]` inherits it
because it inherits the same store.

**What closing it for real would take:** either (a) a two-phase install —
defer any target-side compilation an install hook performs until after
target-side resolution, re-invoking the hook (or a second, later hook) once
an environment is known, which changes the install/resolve ordering this
whole codebase currently treats as fixed; or (b) extend the `c++-abi`
`requires`-shaped check's pattern to `c-abi`/`c-environment` — a package
states the environment its store artifact was built for, checked once the
toolchain resolves, refused on mismatch — which is designed (this section)
but not implemented in this PR. Until one of them lands, the interim
discipline is the same the C++-runtime case already requires: such a
package's install hook must not build more than one environment's variant
into one store directory.

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

### The Standard Library's Own Language Level (mcpp 2026.9.15.2+)

A module graph is compiled at one standard, the root package's
([07 — Workspace](07-workspace.md) §4.2). A package that provides the C++
layer (`hosted-standard-library` or `mcpp:c++-abi=<impl>`) is the one
exception: when it states `[package] standard`, each of its C++ translation
units that neither provides nor imports a module is compiled at exactly that
level, whatever the graph's level is.

```toml
[package]
standard = "c++23"
provides = ["hosted-standard-library", "mcpp:c++-abi=libc++"]
```

A standard library is built at its own level and consumed at every other:
libc++ is compiled at C++23 upstream, and libc++ 22's sources do not compile at
C++20. The exception is safe because the units it covers read and write no
BMI; the package's module units, the `std` and `std.compat` modules included,
stay at the graph's level, so no module is split. The level is appended to
each covered unit's own flags and therefore reaches the compile command, the
dependency scan, `compile_commands.json` and `mcpp emit build-database` alike.
A provider that does not state `standard` is compiled at the graph's level.

The exception is not extended to other packages. A header whose declarations
depend on `__cplusplus` would make an ordinary library's objects and its
consumers' disagree without a diagnostic, and a standard library is the kind of
package whose interface is designed to be consumed at a different level.

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

**When a layer is supplied by the graph, the compiler no longer searches that
layer's host locations.** `mcpp.toolchain.hostflags` reads each layer's origin
and closes the corresponding driver search: a graph-supplied `c-abi` drops the
compiler's own C-library search path (`-nostdlibinc`), and a graph-supplied
`c++-abi` drops its C++ search path (`-nostdinc++`) — both independently of
whether the *other* layer is also the graph's. Before this, a package that
happened to compile only because a host header filled a gap the graph did not
would build on one machine and fail, differently, on another; the two
determined outcomes now are "found in the graph" and "not found", never "found
on this machine's copy of the SDK". A package adapts to this by the layer
predicate below, never by relying on what a host happens to have installed.

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
| **exact triple** | one specific target (also carries `toolchain` / `linkage` / `sysroot` / `runner` / `min_api_level`; see [04 §2.7.3](04-mcpp-toml.md)) | `[target.x86_64-linux-musl]` |

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
  `private_include_dirs` and `std-module-flags` (mcpp 2026.9.1.1+), and
  `runtime` with `frameworks` / `libraries` / `link_library_dirs`
  (mcpp 2026.8.29.1+; `frameworks` since 2026.9.12.3), and `targets.<name>`
  with `kind` (mcpp 2026.9.14.2+; see
  [`targets.<name> kind`](#targetsname-kind--a-librarys-form-on-one-row-mcpp-20269142)).
- **A sub-table that mcpp does not read is reported** (mcpp 2026.9.14.2+): a
  misspelled `[target.<sel>.dependecies]` is a warning naming the sections a
  `[target.<sel>]` table has, and `--strict` makes it an error.
- **A conditional declaration of a dependency replaces the unconditional one**
  (mcpp 2026.9.14.2+). On a row the selector matches, the declaration of an
  identity in `[target.<sel>.dependencies]` is the declaration of that
  identity, and the unconditional declaration does not apply there. A
  dependency that one row links differently is written twice, each time with
  its source:

  ```toml
  [dependencies]
  huxerui.huxerui = { version = "0.3.0" }

  [target.'cfg(env = "android")'.dependencies]
  huxerui.huxerui = { version = "0.3.0", linkage = "shared" }
  ```

  The same rule applies to `dev-dependencies`, `build-dependencies` and
  `feature-deps.<feature>`, and several matching sections apply in manifest
  order, the last one winning. `mcpp why deps` names the table each request
  came from ([09 — Commands by Scenario](09-commands-by-scenario.md)). A table
  that writes options without a source, `huxerui.huxerui = { linkage =
  "shared" }`, declares a dependency on a package named
  `huxerui.huxerui.linkage`: mcpp reports the line with the dependency
  restated, and the resolution fails. An engine before 2026.9.14.2 keeps the
  unconditional declaration.
- **`runtime` is the dialect-neutral half of a link line.** `build.ldflags` is
  spelled the GNU way, and a native `cl.exe` rejects `-L`. These keys say the
  same thing without committing to a spelling: mcpp renders `libraries` /
  `link_library_dirs` as `-L<dir>` + `-l<name>` or `/LIBPATH:<dir>` +
  `<name>.lib` according to the target, and `frameworks` as
  `-framework <name>` on Mach-O rows, nothing on the others. They are the same
  keys `[runtime]` (§2.11 of [04 — mcpp.toml](04-mcpp-toml.md)) already has at
  the top level; this makes them per-target and invents no vocabulary. An
  entry here is appended after the top-level list, never a replacement — the
  way a manifest keeps `UIKit` off the macOS link and `AppKit` off the iOS one
  while sharing every framework both need at the top level. Any other
  `[runtime]` key is reported here and ignored, because the rest are not
  per-target.

  ```toml
  # Linked only on Windows, and spelled correctly for whichever compiler builds it.
  [target.windows.runtime]
  libraries = ["user32", "gdi32"]
  ```

  A predicate whose only content is this table is applied like any other. Until
  mcpp 2026.9.9.1 it was not: the block was parsed and then discarded unless
  something else appeared under the same predicate.
- **A relative search path in `build.ldflags` belongs to the package that wrote
  it.** `-L<dir>` and `-Wl,-rpath,<dir>` — here, in the top-level
  `[build] ldflags`, and from `mcpp::link_flag` — reach the link as absolute
  paths under that package's directory, also when a dependency's flags reach
  its consumer. An entry that begins with a token the loader expands is passed
  as written: `$ORIGIN` and every other `$`-token, and `@executable_path`,
  `@loader_path` and `@rpath` (mcpp 2026.9.14.2+).
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
  matching predicate tables have their flags concatenated, and their dependency
  declarations applied in manifest order. Conditional entries
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

### `abi` — a switch the whole artefact shares (mcpp 2026.9.12.2+)

```toml
[target.'cfg(os = "emscripten")'.abi]
threads    = true
exceptions = true
```

Some properties of a target are not a flag a translation unit may choose. Thread
support is one: on WebAssembly every object, the precompiled standard library
module and the link must agree on shared memory and atomics, and one translation
unit built without them makes the link fail or the module refuse to load.
Exceptions are the same shape: clang records the exception model in a BMI and
refuses an importer that disagrees. Such a property is written as a typed
member of `[target.<selector>.abi]` rather than as a flag in `cxxflags`, so the
engine applies it to every unit that has to agree and compares it with what a
package needs.

| Member | Type | Renders as | Reaches |
|---|---|---|---|
| `threads` | boolean | `-pthread` on a target that is neither PE nor freestanding; nothing on PE and on freestanding targets | the standard library module prebuild, the dependency scan, every C and C++ translation unit of every package, and the link |
| `exceptions` *(mcpp 2026.9.12.3+)* | boolean | `-fexceptions`, on `os = "emscripten"` only, on the compile line through the dialect flags and on the link; nothing on every other target, where exceptions are already the default | the same set `threads` reaches |

Both members enter the dependency cache key through the dialect flags, so a
dependency built without one is never reused by a build with it. An unknown
member, and a member that is not a boolean, are refused naming both `threads`
and `exceptions`.

**Without `exceptions`, the observed failure is at run time, not at link.** A
Web program that throws across an `import std` boundary builds and links with
no complaint — Emscripten's compile-time exception support does not depend on
the flag — and aborts only when the `throw` executes:

```
Aborted(Assertion failed: Exception thrown, but exception catching is not
enabled. Compile with -sNO_DISABLE_EXCEPTION_CATCHING or
-sEXCEPTION_CATCHING_ALLOWED=[..] to catch.)
```

**Only the root manifest decides.** The switch belongs to the artefact, and the
root is the only package that builds one. A dependency that writes
`[target.<selector>.abi]` is reported (`abi/dependency-table`) and changes
nothing. A dependency states what it needs instead:

```toml
[package]
requires_abi = { threads = true }          # the package needs threads

[features]
mt = { requires_abi = { threads = true } } # only this feature needs them
```

A requirement the root does not satisfy is refused before anything compiles,
naming the package and what required the switch:

```
error: `wasmrt` requires the artefact's ABI to have threads (feature `mt`), and this build does not state it.
       Add to the root manifest, for the targets that need it:

           [target.'cfg(os = "<os>")'.abi]
           threads = true
```

Without the refusal the mismatch surfaces as a precompiled-module configuration
error that names neither the package nor the switch.

### `requires_abi` on the target axis (mcpp 2026.9.12.3+)

`[package] requires_abi` and `[features.<f>] requires_abi` are unconditional:
they ask for a switch on every target this package builds for. A dependency
whose need is scoped to a platform — threads on every hosted row, none on the
Web — states that directly on the selector that already carries its sources:

```toml
[target.'cfg(linux)']
requires_abi = { threads = true }

# the per-feature form, named after feature-deps and feature-xlings
[target.'cfg(linux)'.feature-requires-abi]
mt = { threads = true }
```

The requirement set is the union of `[package] requires_abi`, the active
features' tables, and every matching selector's — more than one selector may
ask for the same member, and every one of them is a true statement. It is
unioned only for a selector that matches the resolved target: a requirement
under `[target.'cfg(windows)']` imposes nothing on a Linux build, in either
direction — present or absent, that build is unaffected. A requirement the
root does not satisfy is refused before anything compiles, naming the selector
exactly as written:

```
error: `wasmrt` requires the artefact's ABI to have threads ([target.'cfg(linux)']), and this build does not state it.
       Add to the root manifest, for the targets that need it:

           [target.'cfg(os = "<os>")'.abi]
           threads = true
```

**An engine older than 2026.9.12.3 reads this key silently — neither a warning
nor an error.** `[target.<sel>] requires_abi` is a table-valued key under a
target selector, and the schema sweep an older engine runs skips every
table-valued key on the assumption that a table is the conditional channel;
`requires_abi` there is an inline table, the same TOML shape, and falls
through the same sweep unreported. A package that relies on the refusal above
to protect an unconditional threads build must therefore state its own engine
floor (`[build-dependencies.mcpp] version = ">= 2026.9.12.3"`), rather than
relying on an older client to notice the gap.

`--no-entry` (Emscripten's flag for a module with no `main`) is not a switch
mcpp interprets; it is an ordinary `[target.'cfg(os = "emscripten")'.build]
ldflags` entry, and `main` keeps naming a translation unit regardless — see
[21 — The Target Triple](21-the-target-triple.md#the-wasm-artifact-contract).

### `targets.<name> kind` — a library's form on one row (mcpp 2026.9.14.2+)

```toml
[targets.huxerui]
kind = "lib"

[target.'cfg(env = "android")'.targets.huxerui]
kind = "shared"
```

The per-row form of `[targets.<name>] kind`
([04 — mcpp.toml](04-mcpp-toml.md) §2.2). A framework that is linked into its
application on the desktop rows and must be one shared library on Android
states it once, in its own manifest; every consumer keeps one unconditional
dependency line.

- A row states `kind`, which chooses between the two library forms, `lib`
  and `shared`, or *(2026.9.15.2+)* `linkage`, which states the library's
  default form on those rows without constraining it; the two in one row are
  refused, and a later matching statement replaces an earlier one, including
  the unconditional table's. A name that is not a library target of the package
  (declared or inferred), a program target, or another kind is refused.
- On a matching row the package is constrained to the shared form exactly as
  `[targets.<name>] kind = "shared"` constrains it (`dependency_linkage` in
  [04 — mcpp.toml](04-mcpp-toml.md)): a consumer that writes no `linkage`
  receives the shared library, and a consumer that writes `linkage =
  "static"` receives a warning naming this line, which `--strict` turns into
  an error. `mcpp why deps` reports the form with the reason `row-kind`. A
  row's `linkage = "shared"` gives a silent consumer the shared library with the
  reason `package-default`, and honours a consumer's `linkage = "static"` with an
  information line instead of a warning.
- A selector that names a target-side layer cannot carry it; the table is
  reported and ignored, because a library's form is decided while the graph
  that answers the layer is resolved.
- An engine before 2026.9.14.2 does not read the table and reports nothing. A
  package that relies on it states that engine floor.

## Current limitations

- **A dependency cannot be conditioned on the accelerator.** The accelerator
  layer is resolved from the dependency graph, so a dependency chosen by it would
  decide the answer it is asking for. mcpp reports the predicate and ignores it;
  packages are unconditional or conditioned on the platform, and `[build]
  sources` is what the accelerator selects.
- A layer cannot select a dependency for the same reason, in the general case:
  any derivation earlier than the graph is an inference about a fact that does
  not yet exist.
