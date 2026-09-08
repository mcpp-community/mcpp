# 22 — Features and Capabilities

Features are how a package offers something optional: a compile macro, an extra
source, an extra dependency, or a choice between backends. This chapter is the
reference for declaring them and for consuming them.

Related documents: [05 — mcpp.toml](05-mcpp-toml.md) is the field reference for
the rest of the manifest; [`examples/11-features`](../examples/11-features/) is
a package that declares all three shapes and a test that uses a
dev-dependency; [20 — Heterogeneous Builds](20-heterogeneous-builds.md) is the
largest consumer of the mechanism, because every accelerator lane is a feature.

## `[features]` — Features (Cargo-style, additive)

```toml
[features]
default = ["base"]        # Default activation set
base    = []
docking = ["extra"]       # Activating docking implies activating extra (transitive closure)
extra   = []
```

- Activation sources: the package's own `default` set ∪ explicit requests (the root
  package via `mcpp build --features a,b`; dependencies via the long-form dep spec's
  `features = [...]` / `backend = "..."` sugar).
- Each activated feature gets the macro `-DMCPP_FEATURE_<NAME>` during that package's
  compilation (the name is uppercased and non-alphanumerics become `_`, e.g.
  `backend-a` → `MCPP_FEATURE_BACKEND_A`).
- **strict validation**: when the target package declares a `[features]` table,
  requesting an undeclared feature produces a warning; an error under `--strict`. A
  package that does not declare `[features]` accepts any request (pure macro usage).

### Table form — a feature that contributes more than implied features

A `[features]` entry may be written as a **table** instead of an array, letting the
feature carry package-owned preprocessor `defines`, feature-gated source globs
(`sources`, mcpp 0.0.95+ — the globs leave the default build and compile only when
the feature is active, exactly like an index descriptor's `features.<f>.sources`;
the highest-frequency shape for vendored libraries: *feature = a source set + a
define*), feature-gated per-glob compile flags (`flags`, mcpp 0.0.101+), and/or
capability `requires` / `provides` (see §2.8.1) alongside its implied features:

```toml
[features]
default    = []
# Array shorthand: just implied features.
docking    = ["extra"]
extra      = []
# Table form: contribute a package-owned define when active.
mpl2only   = { defines = ["EIGEN_MPL2_ONLY"] }
# Table form: a define + an implied feature.
fast_math  = { defines = ["APP_FAST=1"], implies = ["extra"] }
# Table form: feature-gated sources + per-glob flags that co-locate with them.
simd       = { sources = ["src/simd/**"], flags = [
                 { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"] } ] }
```

- **The table form accepts exactly** `implies`, `forward`, `defines`, `sources`,
  `flags`, `requires`, `provides`. Anything else is reported as a schema warning
  and ignored (mcpp 2026.9.1.1+); `deps` is reported separately as reserved and
  points at `[feature-deps.<name>]`. Before that release `[features]` was
  the one structured section with no schema check at all, so a misplaced
  `include_dirs` inside a feature built successfully with no diagnostic while
  the identical mistake in `[build]` was reported.
- `defines` are **bare** macro names (no `-D`); each desugars to `-D<x>` on the
  package's own compile when the feature is active — exactly like `[targets.*]
  defines`. They are restricted by convention to the package's **own** namespaced
  macros: a feature does **not** inject free-form package-wide `cflags`/`ldflags`,
  which would break the additive feature-union model. Link flags come from a
  provider dependency (§2.8.1), not from a feature.
- The automatic `-DMCPP_FEATURE_<NAME>` is still defined for every active feature,
  so `defines` are additive to it.
- `flags` (mcpp 0.0.101+) is the same ordered array-of-inline-tables grammar as
  `[build].flags` (§2.3: `glob` required, plus `cflags`/`cxxflags`/`asmflags`/
  `defines`; the `[[features.<name>.flags]]` array-of-tables spelling is accepted
  too, like `[[build.flags]]`). When the feature is active the entries
  are appended **after** the base `[build].flags`, features in name order, so a
  feature rule wins over a broader base rule via "last flag wins"; when it is
  inactive the entries do not exist (no dead-glob warning). This is how a
  feature's group-specific flags co-locate with its `sources` instead of living
  as base rules whose globs go dead on feature-off builds. Unlike `defines`,
  feature `flags` are **private per-TU build flags** — they never propagate to
  consumers (same contract as `[build].flags`), so they stay inside the additive
  model: scoped by glob, deterministic order, no cross-package effect.


### A feature that is a build rule (mcpp 2026.9.7.1+)

Two keys turn a feature into a build rule other packages can use. They are what
lets a consumer write one dependency edge and no build program.

```toml
[features.rules-spirv]
sources           = ["rules/spirv.cppm"]
rule_module       = "mcpp.rules.spirv"
device_extensions = [".comp", ".vert", ".frag", ".glsl"]
```

`device_extensions` states which **device source** extensions this rule
compiles. A consumer that activates the feature gets them classified as device
sources -- never scanned for imports, never a BMI, compiled by something mcpp
does not drive. This is the same shape as `[build] module_extensions`: mcpp
knows what a device source *is* and does not know that `.cu` is CUDA, so a NEW
device language costs no engine change. It is what makes
[20 — Heterogeneous Builds](20-heterogeneous-builds.md)' claim that "a sixth
backend is a package rather than an engine change" true rather than
aspirational; `.slang` was removed from mcpp's built-in table and now arrives
this way.

`rule_module` names the module a consumer's build program imports to reach the
rule, and whose `compile()` it calls. Declared rather than scanned out of the
source, because the program has to be **written** before anything is compiled
and a build that scanned a dependency to decide what to write would order the
two the wrong way round.

Two things follow, and neither puts a package name inside mcpp:

- **`host-module = true` is implied.** A feature naming a rule module has
  already said that is the only way to use it, so the edge does not repeat it.
- **A package with no `build.mcpp` gets one.** mcpp writes the program those
  rules describe into the build directory and compiles that. A package with its
  own program keeps it: the synthesis fills an absence and never overrides, and
  the generated file is the program a project would have written, so taking it
  over is a copy and an edit.

The feature is still requested **by name**:

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-spirv"] }
```

An earlier design derived the set from the extensions a project's sources
carried. It was withdrawn because two packages may claim one extension -- a
third-party CUDA rule is a thing someone will write -- and because a manifest's
job is to describe the build, which a derived feature set no longer does.

Both keys must appear together. One without the other is a declaration nothing
can act on, and it is refused at parse time rather than in a consumer's build.


## `provides` / `requires` — Capabilities (backend selection)

A **capability** is a shared abstract name (e.g. `blas`). A package can *provide*
one; a feature can *require* one instead of naming a concrete package, and the
resolver binds exactly one provider from the dependency graph. This is how a build selects
one of several interchangeable backends (OpenBLAS / MKL / …) without baking a choice
into the library.

```toml
# A provider package satisfies a capability for any dependent that requires it.
[package]
name     = "compat.openblas"
version  = "0.3.0"
provides = ["blas", "lapack"]
```

```toml
# A consumer requires the abstract capability via one of its features.
[features]
use_blas = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }

# When >1 provider is in the graph, pick one (else the build errors and lists them).
[capabilities]
blas = "compat.openblas"     # equivalently: mcpp build --cap blas=compat.openblas

[dependencies]
compat.openblas = "0.3.0"    # the provider must be a real dependency in the graph
```

The reserved prefix `mcpp:` names the target-side layers this engine resolves,
and those names are validated against a closed set. A package-level `requires`
array carries the symmetric statement — what a target-side layer must resolve to
for this package to be usable.

```toml
[package]
name     = "acme.llvm-runtime"
version  = "0.1.0"
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]
```

A package that is a standard library states its `std` module source under
`[build]`, where the flags it needs become conditional like any other build
input.

```toml
[build]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc++"]

[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]
```

See [14 - The Target Side](14-target-side.md) for the five layers, the rules
that govern them, and the diagnostics.

Binding is **deterministic**:

| Providers of a required capability in the graph | Result |
|---|---|
| exactly one | bound automatically (no config needed) |
| a `[capabilities]` pin / `--cap` names one | the pin wins |
| zero | **error**: no package provides `<cap>` |
| two or more, unpinned | **error**, listing the candidates — never a silent guess |

The bound provider's link/include flags reach the consumer through normal
dependency mechanics; the capability layer is the *selection-and-validation* step
that turns a silently-wrong or missing backend into a loud configure-time error.

**Binding selects a provider; it does not prune the link line.** A dependency
package contributes its object files to the consumer's link regardless of whether
its capability was the one bound. Measured with two packages that both provide
one capability and both define `cap_probe`: unpinned, resolution fails as the
table says; after pinning one with `[capabilities]`, the build reaches the linker
and fails there instead —

```
ld: obj/mcpplibs_pa/src/impl.o: in function `cap_probe':
    multiple definition of `cap_probe'; obj/mcpplibs_pb/src/impl.o: first defined here
```

This matters for a capability whose providers define the **same symbols** — a
whole-program singleton such as `operator new`, or a C API with one fixed name
set. For those, two providers in the graph is a defect to fix rather than an
ambiguity to pin: pinning replaces an error that names both candidates with one
that names a mangled symbol. Interchangeable *libraries* (BLAS implementations,
which export distinct symbol sets and are selected per link) are unaffected.

### `exclusive` — a package declaring it is the only provider

The paragraph above describes a defect the engine cannot detect. Seeing that two
providers define the same symbols requires their object files, which do not
exist when capabilities are bound; and refusing every duplicate provider as a
rule would break the BLAS case in the same paragraph, which is legitimate.

So the package says it:

```toml
[package]
name      = "compat.cublas"
provides  = ["gpu-blas"]
exclusive = ["gpu-blas"]
```

Two packages that both provide `gpu-blas`, where at least one declares it
exclusive, are refused when capabilities are bound — before anything is
compiled, naming the capability and both providers:

```
error: capability 'gpu-blas' is provided by more than one package, and they
       declare it EXCLUSIVE.
         providers: [compat.cublas, compat.rocblas]
         exclusive: [compat.cublas, compat.rocblas]
       Two implementations of one interface define the same symbols, so the
       link would resolve every call to whichever archive it reached first.
       Keep one of them — a `[capabilities]` pin selects a provider for a
       REQUIREMENT and cannot make two definitions of one symbol safe.
```

The refusal reports `exclusive-capability` in `--format json` (chapter 11).

### `version-floor` — needing more of the machine than it has

Some facts about a machine bound what may be built for it, and the failure when
they are ignored arrives late: a program built against a runtime newer than the
driver it will meet links cleanly and fails at first use, naming neither side.

A package states what it needs:

```toml
[[runtime.requirements]]
kind  = "version-floor"
value = "cuda.driver >= 12.0"
```

and a package that established a fact about this machine — at install time,
which is where probing belongs — states it:

```toml
[runtime]
provides = ["cuda.driver=12.4"]
```

mcpp compares them when capabilities are bound and refuses before anything is
compiled, reporting `version-floor-unmet`:

```
error: `toolkitnew` requires cuda.driver >= 13.0, and this machine has 12.4.
         stated by: driverfact
```

**No vendor vocabulary reaches the engine.** It reads a name, a relation and a
version; `cuda.driver` is data passing through, and a backend mcpp has never
heard of compares the same way.

**A floor nobody answered is silent.** A machine that never declared what it
has is not a machine that fails the floor — it is one nobody asked. Turning
"we do not know" into "no" is the failure mode this exists to avoid, and it is
asserted directly: `tests/e2e/603_version_floor.sh` builds a project whose floor
names something no package provides.

**It is a claim about this package's own symbols**, so an entry that names a
capability the package does not provide is reported as a schema warning: there
is nothing to be exclusive about. And a capability nobody declares exclusive
behaves exactly as before — two BLAS implementations still coexist, and the
existing "two or more, unpinned" error still applies only when something
*requires* the capability.

## `[feature-deps.<name>]` — dependencies a feature pulls in

A dependency declared under `[feature-deps.<name>]` is **optional**: it is
resolved only when that feature is active (root `--features`, or a dependency
spec's `features = [...]`). A dependency in `[dependencies]` is always resolved;
optionality is expressed by *where* the entry is declared, not by a flag.

```toml
[features]
use_blas         = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }
backend-openblas = { implies = ["use_blas"] }

# Pulled ONLY when `backend-openblas` is active. Each entry is a full dependency
# spec (version/path/git + its own features).
[feature-deps.backend-openblas]
compat.openblas = "0.3"
```

**`"^0.3.0"` and not `"0.3.x"` or `"0.3"`.** Measured against a package the
index certainly carries, with a **build** as the criterion:

| Written | Result |
|---|---|
| `cmdline = "0.0.1"` | builds |
| `cmdline = "^0.0.1"` | builds |
| `cmdline = "0.0"` | resolves, then `install path missing after fetch` |
| `cmdline = "0.0.x"` | `E_NOT_FOUND`, naming the package — which exists |

The three outcomes are worth distinguishing, because two weaker criteria each
admit a form that does not work: "no `E_NOT_FOUND`" admits the two-segment
prefix, and "resolves" admits it as well. Only building against the real index
settles it.

This matters more here than in `[dependencies]`. A feature whose
implementation cannot be fetched is a feature that does not exist, and a project
using a **path** dependency during development never consults the index — so the
failure appears only after publication, to somebody else.

This composes with capabilities (§2.8.1): a single `backend-openblas` feature
both **pulls** the provider (`compat.openblas`, which `provides = ["blas"]`) and
**turns on** the consumer switch (`implies = ["use_blas"]`, which
`requires = ["blas"]`). With one provider in the graph the capability binds
automatically — `features = ["backend-openblas"]` is all the consumer writes.

In an index package's Lua descriptor the same is written inline:

```lua
features = {
    use_blas         = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },
    ["backend-openblas"] = {
        implies = { "use_blas" },
        deps    = { ["compat.openblas"] = "0.3.x" },
    },
}
```

### A default implementation that stays replaceable

The same three pieces cover the case where a library wants to *offer* an
implementation without *imposing* one — a whole-program singleton such as
`operator new`, a logging sink, or a panic handler:

```toml
[features]
default   = []
# The consumer-side switch: "I use the part of this library that needs an allocator".
alloc     = { requires = ["freestanding-allocator"] }
# The built-in default: activating this one is enough.
alloc-kal = { implies = ["alloc"] }

# Resolved only when `alloc-kal` is active, so the library itself carries no
# dependency on the implementation.
[feature-deps.alloc-kal]
std-freestanding-alloc-kal = "0.1.x"
```

Three usages, one line each:

| Consumer needs | What the manifest says |
|---|---|
| none of the allocating parts | `std-freestanding = "0.2.0"` — no allocator enters the graph |
| the default | `features = ["alloc-kal"]` — the implementation arrives with it, and its package name never has to be known |
| its own or a third party's | `features = ["alloc"]` plus a package that `provides = ["freestanding-allocator"]` |

Two properties make this preferable to shipping the implementation
unconditionally. A library that ships one takes a decision belonging to the
program, and it cannot be undone: features are **additive**, so there is no way
for a consumer to switch a default *off*. And because a dependency package's
objects link unconditionally (§2.8.1), a shipped default plus a program-supplied
one is a duplicate definition rather than a replacement — the archive semantics
that let a C++ standard library offer a replaceable `operator new` do not apply
to a package dependency. Keeping the implementation behind a switch means the
two never coexist.
