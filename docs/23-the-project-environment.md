# 23 — The Project Environment

**Reader:** an author whose build needs tools that are not the compiler.

**The question this chapter answers:** how does a project declare the
environment its build runs in, and what does that declaration decide.

**Not here:** what a build program does with those tools, which is
[30 — Build Programs](30-build-mcpp.md), and choosing a compiler, which is
[20 — Toolchain Management](20-toolchains.md).

A project can declare the environment it builds in. That one declaration
decides which C library the project links against and which tools its build
programs find — so a `mcpp.toml` means the same build on a developer's laptop
and in CI, whatever else those two machines happen to have installed.

```toml
[xlings]
subos = "tools"

[xlings.workspace]
"xim:qemu-riscv" = "9.2.4-1"
```

Working project: `examples/07-project-subos/`.

## 1. The definition of a SubOS

A SubOS is a directory that holds a userspace: its own `bin`, its own library
view, its own installed package versions, and a `subos_info` block describing
itself. mcpp treats it as the answer to "what does this project build
against", and it is the only mechanism that answers that question — not the
compiler's path, not `XLINGS_ACTIVE_SUBOS`, not the shell.

Two kinds exist, and the difference is where the directory lives:

| Declaration | Directory | Shared with |
|---|---|---|
| none | mcpp's initialized `subos/default` | every project on the machine |
| `subos = "default"` | the same directory, named explicitly | every project on the machine |
| `subos = "<name>"` | `<project>/.mcpp/.xlings/subos/<name>/` | nothing |

The third row is the isolated one. It belongs to the project, it sits beside
the manifest, and removing the project removes it.

## 2. The scope of the declaration

**The C library.** A payload-first build links against one specific glibc, and
which one is a fact about the project rather than about the machine. Chapter 8
covers the binding, the degradation rules, and what a SubOS that does not
describe itself does to them.

**The tools a build program sees** (mcpp 2026.8.25.1+). The declared
environment's `bin` goes to the front of the `PATH` that `build.mcpp` runs
with:

```
PATH=<the declared environment's bin>:<the PATH mcpp itself was started with>
```

A build program that spells `qemu-system-riscv64` as a bare name therefore gets
the copy inside the declared environment. Chapter 7 covers the contract this
rides on.

**Only for projects that declare one.** A project with no `[xlings].subos`
gets the `PATH` mcpp was started with, byte for byte. Putting a shared
directory in front of every project would make what a build sees depend on what
else had been installed on that machine — two projects on one machine would
agree with each other, and the same project on two machines would not.
Declaring it is what puts it there.

**Prefixed, not replaced.** A build program legitimately calls `git`,
`python3` or a shell, and none of those live in a SubOS. Front position makes
the declared environment the default answer; everything else stays reachable
behind it.

### 2.1 The version pins that apply (2026.9.3+)

Naming an environment also changes where a tool's version comes from. A
project's own `[xlings.workspace]` entries always win — over the environment
here, and over a dependency's declaration by the rule in section 3 — and what
differs is what they are laid over:

| The project declares | The version of a tool it did not name comes from |
|---|---|
| `[xlings.workspace]`, no `subos` | the machine's environment |
| `[xlings.workspace]` and `subos = "<name>"` | that environment's own workspace; the machine's does not apply |

The second row is what isolation means. A named environment has its own
installed set, and carrying the machine's versions into it would name versions
that are not there — so a project that relied on the machine's tools has to
declare them once it names an environment.

An `xlings use` performed inside the project outranks both, until mcpp rewrites
the environment: it is the layer merged last, and an action a person took
should beat a file.

## 3. Outside the declaration’s scope

`[xlings.workspace]` names packages to be present in the environment, and each one's
payload directory is delivered separately as `MCPP_XPKG_<NAME>_DIR`. That is a
different question from `PATH` and stays a different answer: a build program
that needs a package's data files (protoc's well-known `.proto` files, say)
asks for the directory, and one that needs to *run* a program asks `PATH`.

**A workspace member's declaration is not the workspace's.** In a workspace
build the workspace root owns the selection; a member's `[xlings]` applies only
when that member is built as an independent root.

**A dependency's declaration is a different matter, and it is honoured**
(2026.9.5.4+ for `[xlings] deps`, 2026.9.6.6 for the version rule below). A
board-support package knows which emulator reaches its machine, and a rule
package knows which toolkit its rule drives; a consumer that had to repeat
either is the duplication such packages exist to remove. What the dependency
declares is installed, and `MCPP_XPKG_<NAME>_DIR` answers for it in that
dependency's own build program.

Where a project and a dependency name **one package**, one version of it is
installed: identity is `(namespace, name)` and the version is a constraint on
it. The declaration nearer the artifact wins and the override is reported;
a pin that fails a requirement the other side stated is refused naming both.
See *One package, one version* in [23 — The Project Environment](23-the-project-environment.md).

## 4. Reading an environment, never creating one

mcpp resolves a declared name and reads what it finds. A name that does not
resolve is a hard error:

```
error: selected SubOS 'tools' does not exist at …/.mcpp/.xlings/subos/tools;
create/bootstrap that environment instead of falling back to active/default
```

Falling back to the default or to whatever is active would substitute a
different environment for the one the manifest named, which is precisely what
would make one `mcpp.toml` mean two different builds. Creating and populating
a SubOS is xlings' layer — `xlings subos new` — and mcpp managing SubOS state
would invert that layering.

An environment that exists but carries no `subos_info` block **degrades rather
than fails**: the runtime binding reports `inconclusive`, no payload-first
binding is available, a note is printed, and the build continues. Chapter 8
gives the full rule.

## 5. The case for a private environment

- **A generator whose version changes what it emits.** `protoc`, `flatc`, a
  shader compiler: the output is an input to everything downstream, so the
  project pins the producer instead of hoping the machine has a compatible one.
- **An emulator a build program runs.** Several bare-metal packages boot an
  artefact under QEMU as part of proving it works; which QEMU is part of what
  was proven.
- **A project whose CI and developer machines differ**, where neither is wrong
  and the build must not notice.
- **Two projects on one machine that need different versions of one tool.**
  Sharing a directory means one of them loses; a private environment means the
  question does not arise.

Against that: an isolated environment is a directory that has to be created and
populated, and the first build pays for it. Since 2026.8.29 mcpp does that
work — a declared `[xlings.workspace]` entry is provisioned on first use, and a named
`[xlings] subos` that does not exist yet is created rather than refused — but
the cost is real: the first build on a clean machine downloads and installs
before it compiles anything. A project whose tools are ordinary and whose
versions do not matter is better off declaring nothing and inheriting the
machine's.

Under `--offline` / `MCPP_OFFLINE` or `MCPP_NO_AUTO_INSTALL`, mcpp refuses
instead of installing, and names the packages so they can be provisioned
out of band — the same two knobs `[toolchain]` honours, for the same reason: an
unasked-for download is not something a build decides on a project's behalf.

The declaration is provisioned on every host that builds the project, and a
package the host cannot install is an error, not a skipped entry. A tool that
exists for one host platform only is therefore declared for that platform
(2026.9.2.1): `deps = [{ linux = "qemu-user-aarch64" }]` declares the emulator
on Linux and nothing elsewhere. The keys and the resolution rule are in
this chapter.

**Which verbs install it.** An entry may name a tier —
`{ version = "0.24.0", when = "run" }` — and a `[feature-xlings.<feature>]`
table gates one on a feature. A tool the project will not use is then not
downloaded: this chapter. Omitting the tier is the historical behaviour.

**The runner.** A program under `[xlings.workspace]` is also where
`[target.<triple>].runner` looks first for its first element, before `PATH`
([04 §2.7.3](04-mcpp-toml.md)). The two keys together provision a user-mode emulator on a
CI host and execute a cross-built artifact through it, without the manifest
naming the payload's path.

## 6. Declarations that belong elsewhere

| Need | Declared in |
|---|---|
| a library the program links | `[dependencies]` |
| the compiler | `[toolchain]`, chapter 3 |
| a host tool a dependency produces | `tools = [...]`, chapter 7 |
| a tool present in the environment | `[xlings.workspace]` |
| a tool only one verb or one feature needs | `when = "run"`, `[feature-xlings.<f>]` |
| which environment | `[xlings] subos` |

## 7. `[xlings]` — the manifest keys

```toml
[xlings.workspace]                 # what this project's environment contains
cmake                    = "3.28"
"xim:picolibc-riscv"     = "1.8.12"        # a namespaced package - quotes required
code                     = ""              # present; version unconstrained
llvm                     = { macosx = "20", default = "22" }
```

```toml
[xlings]
subos = "dev"                      # a named, isolated environment
```

`[xlings]` is mcpp's surface for **xlings' local project mechanism**: the
project `.xlings.json` that gives a directory its own environment. The
subsection names and their meanings are that file's, and mcpp materializes them
into `<project>/.mcpp/.xlings.json` with no translation layer.

**`[xlings.workspace]` is the one table.** An entry names a package and the
version this project uses it at. mcpp provisions it — installing it when the
machine does not have it, mapping it when it does — and materializes it as a
resolution pin, so the version the project named is the one its tools resolve
to.

### Writing an entry

| Form | Means |
|---|---|
| `cmake = "3.28"` | that version |
| `llvm = "22"` | the highest installed `22.*`; a version prefix resolves |
| `code = ""` | present, version unconstrained |
| `"xim:picolibc-riscv" = "1.8.12"` | a package from the `xim` index |
| `llvm = { macosx = "20", default = "22" }` | per host platform |

**A namespaced package is written `"<namespace>:<name>" = "<version>"`, and the
quotes are required** — a TOML bare key cannot contain a colon. That is the
recommended form and the one every official package uses: an entry names a
package and then says which version of it, so the namespace belongs to the
name.

The namespace is also accepted on the version (`picolibc-riscv = "xim:1.8.12"`),
because that is what the materialised `.xlings.json` carries — a key there is an
xvm target and the scope qualifies the version. Two vocabularies, one entry.
Writing it on both halves with different values is an error, and so is naming
one package twice under two spellings.

Platform keys are xlings' own — `linux`, `macosx`, `windows` — plus `default`.
`macos` and `macosx` are the same platform written in two vocabularies (mcpp's
triples say one, descriptors and xlings' project file say the other) and both
are accepted wherever a platform is named. A table with no key for this host
and no `default` declares nothing here.

### Two resolution axes — host and target (mcpp 2026.9.6.4+)

A tool entry answers one of two different questions, and the table it is written
in decides which:

| Written | Axis | Resolved against |
|---|---|---|
| `[xlings.workspace]`, platform keys in the value | host | the machine running the build |
| `[target.<selector>.xlings.workspace]` | target | the resolved target (`--target`, else the host) |

Both are correct spellings and neither supersedes the other. A tool that
executes on the build machine belongs on the host axis; a payload the produced
code is compiled or linked against belongs on the target axis.

```toml
[xlings.workspace]
"xim:dpcpp" = "7.1.0"              # a compiler, and it runs here

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:glibc"         = ""           # what the device units are compiled against
"xim:linux-headers" = ""
```

On a native build the two axes name the same platform, so a project that states
target facts on the host axis is right by accident and keeps working. It stops
being right the first time that project is cross-compiled. **For anything the
produced code is compiled or linked against, the target axis is the recommended
form.**

`[target.<selector>.feature-xlings.<feature>]` composes the condition with the
gate, exactly as `[target.<selector>.feature-deps.<feature>]` does: the selector
says which targets, the feature says whether at all.

```toml
[target.'cfg(os = "linux")'.feature-xlings.backend-vulkan]
"xim:shaderc" = "2026.3"
```

**A selector here must not name a RESOLVED layer.** `c-abi`, `c++-abi`,
`compiler`, `compiler-runtime` and `kernel-abi` are answered by dependency
resolution, which happens after tools are installed and after build programs
run. A tool conditioned on one would be declared and never installed — a build
that succeeds with the tool simply absent — so such a manifest is refused,
naming both the tool and the predicate. Condition it on the target, or gate it
on a feature: `[feature-xlings.<feature>]` is known before anything is
provisioned.

**`accelerator` is the exception, and is admitted** (mcpp 2026.9.6.5). It is
not resolved from anything: it is `--accel`, or `[build] accel`, read before the
first package is looked up. A payload predicated on it is merged in the same
pass as a triple predicate and installed like any other.

```toml
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"
```

This is the form a project with a device island should use. Without it the
vendor toolkit is declared unconditionally or not at all, so `mcpp build` with
no accelerator — the cheapest build, and the one CI usually runs — downloaded
gigabytes for a device it was not compiling for.

The same rule governs dependencies: `[target.'cfg(accelerator = "cuda")'.dependencies]`
is honoured, while a dependency conditioned on a resolved layer is not, because
that one would decide the answer it is asking for. Nothing about the
accelerator is circular.

The selector is the only place the condition is written. A value under a
selector that also carries platform keys states one fact twice, and is refused
naming both halves:

```
[target.cfg(os = "linux").xlings.workspace] xim:tool: the value carries platform
keys (linux, macosx), but [target.cfg(os = "linux")] already says which targets
this applies to.
```

`subos` is not conditional on a target: a project has one environment, so
`[target.<selector>.xlings]` refuses the key rather than dropping it.

**A published descriptor carries no edge for a target-axis entry**, and
`mcpp publish` says so. A descriptor has one block per platform, and a selector
is not a platform — `cfg(target_arch = "aarch64")` names no block that file has.
What a CONSUMER of the package gets installed comes from the top-level
`[xlings.workspace]`; the target axis stays correct for what the package's own
build compiles against.

See [SPEC-004](specs/manifest-semantics.md) for the general rule these two axes
are an instance of.

### `when` — the verbs that need this tool (mcpp 2026.9.4.2+)

```toml
[xlings.workspace]
"xim:qemu-arm"  = "9.2.4-1"                             # every build, as before
"xim:codegen"   = { version = "1.0",    when = "build" }
"xim:probe-rs"  = { version = "0.24.0", when = "run"   }
"xim:clang-tidy"= { version = "20",     when = "dev"   }
```

Package dependencies have had this axis since the beginning —
`[dependencies]`, `[build-dependencies]`, `[dev-dependencies]`. Tools had one
list, so a board-support package that named both an emulator and a debug probe
installed both for every consumer, including one that only wanted the library
to compile.

| `when` | Installed by | Reaches a consumer |
|---|---|---|
| *(omitted)* | every verb that builds | yes |
| `build` | every verb that builds | yes |
| `run` | `mcpp run`, `mcpp test` | yes |
| `dev` | only the package that declared it, as the root | **no** |

**Omitting `when` is the pre-2026.9.4.2 behaviour exactly**, so no manifest has
to change. Narrowing is optional; it is not a question an author has to answer.

`dev` is the only tier that does not propagate. It means *"while the package
that declared this is itself being developed"*, so a dependency's `dev` entry is
never installed for a consumer. Every other tier does reach one, which is the
point of a board package knowing its own machine: it declares the emulator once
and every consumer gets it.

The tier is written on the entry rather than as a second table, on the same
reasoning that makes `[dependencies]` accept both `dep = "1.0"` and
`dep = { version = "1.0", features = [...] }`. A scoped entry must name
`version` even to leave it empty (`version = ""` means *present, any version*),
because `{ when = "run" }` and a misspelt `version` key would otherwise be
indistinguishable.

### `[feature-xlings.<feature>]` — a tool a feature needs

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}

[feature-xlings.hardware]
"xim:probe-rs" = "0.24.0"
```

The same table, gated on a feature, spelled the way `[feature-deps.<feature>]`
is. A consumer who never asks for `hardware` never downloads a probe driver.
Entries here accept `when` exactly as the unconditional ones do.

A feature name no `[features]` table declares is reported as a schema warning:
it activates for nobody and installs nothing, and a tool whose absence is only
visible as *"the device is never reachable"* is the hardest kind to diagnose.

### A rule package brings its own environment (2026.9.6.6+)

The table above is what a project writes when it has an opinion. Most projects
have none, and nothing is what they write:

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-cuda"], host-module = true }
```

That one edge is the whole declaration. The rule package names the packages its
rule needs and the version it needs them from, under the feature that selects
it and the accelerator it is for:

```toml
# in the rule package, not in your project
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-cuda]
"xim:cuda-nvcc"   = ">=12.9.86"
"xim:cuda-cudart" = ">=12.9.79"
```

Two gates, and both must open. The feature says *whether this rule is wanted*;
the selector says *which builds actually download it*. A CPU-only build of the
same project opens neither and installs nothing.

Which package, and how old it may be, is the rule author's knowledge. Repeating
it in every project that uses the rule is a copy that goes stale silently — the
rule moves and the projects do not.

### One package, one version (2026.9.6.6+)

A tool address is `[<ns>:]<name>[@<version>]`, and its **identity is the
`(namespace, name)` pair**. The version is a constraint on that package, never
part of its name, so `xim:glibc`, `xim:glibc@2.40` and `xim:glibc@>=2.38` all
name one package. One version of it is installed per build.

Which one is decided in two steps.

**Adjudication — the declaration nearer the artifact wins.** Your project
outranks a package it depends on, so a pin overrides a rule's requirement:

```toml
# the project, when it does have an opinion
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc" = "13.3.33"
```

A declaration that names no version abstains: it says the package is wanted and
nothing about which version, so it cannot outrank a floor merely by being
nearer. When two declarations disagree and both name a version, mcpp reports
which was used — an override visible only as *"two versions were declared and
one directory exists"* is a fact the reader has to reconstruct from the
filesystem.

**Validation — the winner must satisfy every requirement that lost.** `>=`,
`^`, `~` and comma-combined forms are requirements. A pin that fails one is
refused, naming both sides:

```
error: `xim:cuda-nvcc` is pinned to 12.0.0 by this project, and mcpp:plugins
       requires >=12.9.86.
       One version of a package is installed, so the two cannot both hold.
       fix: pin a version satisfying >=12.9.86, or drop the pin and let the
       requirement decide.
```

A bare version is a *choice*, not a requirement: two exact pins that differ are
adjudicated and reported, not refused. Only a stated requirement can be
violated.

**This is a comparison, not a search.** The version is chosen by adjudication
and then checked, so mcpp never has to ask the index which versions exist and
carries no constraint solver. The cost is stated rather than hidden: a
combination a solver could satisfy — a project's `>=8.0`, a rule's `8.5.0`, and
8.3 as the newest in the index — is refused instead, and the refusal says how to
proceed.

Ranges are resolved in both directions. `>=2026.1` installs the highest
published version satisfying it, `>=2099.1` is refused as unsatisfiable, and
`mcpp::xpkg_dir` answers with the highest **installed** version satisfying the
range — a rule that declares a floor can find what the floor brought in.

### Which version a tool the project did not name resolves to

| The project declares | The version comes from |
|---|---|
| `[xlings.workspace]`, no `subos` | the machine's environment, with the project's own entries laid over it |
| `[xlings.workspace]` and `subos = "<name>"` | that environment's own workspace; the machine's does not apply |
| neither | the machine's environment |

The middle row is not an omission. A named environment has its own installed
set, and carrying the machine's versions into it would name versions that are
not there. Naming one is how a project asks for isolation; leaving it out is
how it asks for the machine's environment with its own entries on top.

An `xlings use` performed inside the project outranks this table until mcpp
rewrites the environment, because it is the layer merged last.

### `deps`, superseded

`deps = ["xim:qemu-riscv@9.2.4-1"]` is the pre-2026.9.3 spelling of the same
statement. It is still honoured and is reported once, with the
`[xlings.workspace]` line to write instead. It is not refused, because a
refusal would reach a *dependency's* manifest, which a project that pinned an
exact version of that package cannot edit.

### `envs`, removed

`[xlings.envs]` was materialized into `.xlings.json` and read by nothing: a
program's environment is declared by its own package, and an environment's by
that environment. The key is now an error naming both. Nothing in the index
used it.

## 8. Related chapters

- [30 - build.mcpp](30-build-mcpp.md) — the contract a build program receives,
  including the `PATH` it runs with.
- [91 - Toolchain Internals](91-toolchain-internals.md) — runtime selection,
  the `RuntimeBinding` snapshot, and the degradation rules.
- [04 - mcpp.toml](04-mcpp-toml.md) — the rest of the manifest.

## Current limitations

- **A tool cannot be conditioned on the accelerator.** The accelerator is
  resolved after the dependency graph, so such a tool would be declared and never
  installed — a build that succeeds with the tool simply absent. A manifest that
  writes one is refused.
- The declaration nearer the artifact wins, and the override is reported. A pin
  that fails a requirement the other side stated is refused naming both sides
  rather than installed alongside it.
