# The build-rule package: identity enforced, shape documented

> Status: design. Not implemented.
> Scope: packages consumed through `host-module = true` — build rules, or
> plugins — and, in section 12, the proposal to give mcpp's own source tree the
> same treatment by moving separable units into `modules/`.
> Supersedes: the 2026-08-05 extensibility architecture, section 10.1 row 4
> (`mcpp.rules.*` as a reserved prefix), which cannot hold and never did.
> Relates to: #355 (host tools), #359 (re-exported provisions).
> Touches (estimate): `src/build/prepare.cppm`, `src/build/hostprogram.cppm`,
> `src/build/provisions.cppm`, `src/manifest/toml.cppm`, `docs/05-mcpp-toml.md`,
> `docs/07-build-mcpp.md`, and the Chinese counterparts of both.

---

## 0. Summary

The mechanism for distributing a build rule as an ordinary package landed in
2026.8.5.1 and works. What is missing is the specification: what a rule package
is allowed to be called, what it may depend on, and what shape its interface
should take. One rule package exists in the index today —
`mcpplibs::grpcgen 1.83.0`, 491 lines, out of 132 descriptors — so every trait
of the abstraction currently rests on a single sample.

This document separates the rules into two tiers, because the two tiers have
different failure modes:

- **Identity is enforced by the engine.** Names, collisions, the boundary
  between a rule and the target it serves. These admit criteria, so they must
  produce diagnostics rather than conventions.
- **Shape is documented for authors.** Layering, entry points, which
  diagnostic channel to use. These admit no criteria; pretending otherwise
  would produce a checker that is wrong more often than the author is.

The split is not a matter of taste. In the previous round of this work exactly
one rule was written as a pure convention — `mcpp.rules.*` as a reserved
prefix — and it is the one rule that was silently abandoned during
implementation, without the design document being amended.

The document also records a finding that changes the scope of the work:
**`[build-dependencies]` is parsed, merged, propagated through workspaces and
conditionalised by target predicate, and no code path reads it to make a
decision.** A rule package therefore has no way to speak for itself about what
it needs at build time, which is the real reason a rule is a leaf. The leaf
constraint is not a policy that was chosen; it is a declaration site that was
never wired.

---

## 1. What the engine enforces today

These are facts, not proposals. The specification must explain them and must
not contradict them.

| # | Behaviour | Evidence |
|---|---|---|
| 1 | The module name registered for a host module is the dependency's bare `package.name`; the namespace is discarded | `prepare.cppm:4780` — `auto const& canon = depPkg.manifest.package.name;` |
| 2 | `package.name` must be a single atomic segment and may not contain `.` | `docs/spec/package-identity.md:90` |
| 3 | A host module must have an interface unit at its lib root; a missing one is a hard error | `resolve_lib_root_path`, diagnostic `"host module 'x': no interface unit at …"` |
| 4 | The rule interface is compiled alone, so it may import `std` and the bundled `mcpp` module and nothing else | `hostprogram.cppm:423-425` |
| 5 | It is compiled with the same flag set as `build.mcpp`, which is what makes BMI agreement structural rather than checked | `hostprogram.cppm:415-420`, and section 10.5 of the 2026-08-05 document |
| 6 | A package reached only by `host-module` edges has its source globs emptied, so it is not compiled into or linked with the target | `prepare.cppm:4820-4826` |
| 7 | A provision crosses one further edge only when that edge declares `reexport = true` | `provisions.cppm:81` |

Two consequences of (1) and (2) taken together are load-bearing for the rest of
this document. First, `import mcpp.rules.protobuf;` cannot be written: the
module name is the package name, and the package name may not contain a dot.
The reserved-prefix decision was therefore unimplementable from the moment it
was recorded. Second, module names live in a flat space with no namespace
component, so two rule packages from different namespaces can collide, and
nothing detects it: `env.hostModules` is a plain vector
(`build_program.cppm:101`) iterated at `:577`, `:661` and `:792` with no
de-duplication and no collision check.

---

## 2. Tier 1 — identity, enforced by the engine

### I1. The module name is declared by the rule's source, not derived from the package name

The codebase has already written this rule down for ordinary packages:

> `prepare.cppm:3924` — *"Module names are authored API and are not required to
> mirror package identity."*

The implementation exists. `declared_modules_for` (`prepare.cppm:3651`) reads a
package's sources and returns the module names they declare, via
`declared_module_roots` (`mangle.cppm:114`). Dotted names survive intact:
`is_name_cont` (`mangle.cppm:79`) accepts `.`, so `export module
mcpp.build.protobuf;` yields the whole string rather than its first segment.

`prepare.cppm:4780` is the single place in the codebase that contradicts the
rule the codebase states. Replacing it with a scan of the lib-root interface is
a return to consistency, not the addition of a special case.

The ordering works: registration happens at `:4780` and the source globs of
rule-only packages are emptied at `:4820`, so the globs the scan needs are
still populated when the scan runs.

**The change breaks nothing, and it fixes a portability trap.** How
`logicalName` binds to the BMI differs by compiler, and the three branches of
`build_host_module` do not agree:

| Compiler | Binding | A declared name differing from the package name |
|---|---|---|
| MSVC | `/reference <logicalName>=<ifc>` (`hostprogram.cppm:580`) | the import fails |
| Clang | `-fmodule-file=<logicalName>=<pcm>` (`hostprogram.cppm:634`) | the import fails |
| GCC | `out.useFlags = {"-fmodules"}` (`hostprogram.cppm:647`); BMIs are implicit under `<cwd>/gcm.cache`, keyed by the **declared** name | it works |

So a rule package whose lib root declares a name other than its package name
builds on GCC and fails on Clang and MSVC today, with no diagnostic explaining
why. I1 removes that asymmetry: the registered name becomes the declared name
on every compiler, GCC's behaviour is unchanged because nothing named the module
there in the first place, and the two other branches stop being handed a name
the source never used.

For a package where the two names already agree — the only kind that works on
all three compilers today, and the only kind in the index — the scanned name and
the registered name are identical. `grpcgen` confirms it by measurement:
`export module grpcgen;` with `name = "grpcgen"`.

### I2. Deleted

With the package name no longer carrying the module name, the requirement that
a package name be a legal C++ module name disappears. `grpc-rules` becomes a
legal package name again, provided its lib root declares a legal module name.
Legality is enforced by the compiler, which owns that rule; mcpp does not need
a second copy of C++ naming rules that can drift from the first.

### I3. Duplicate module names among the host modules of one build are a hard error

Decoupling the module name from package identity makes collisions possible, so
I1 and I3 must land together. The check is scoped to what the developer
actively declared for this build — the host-module set visible to one
`build.mcpp` — and not to the index. The diagnostic names both packages by
fully-qualified identity and gives each interface path, because the two
packages may otherwise be indistinguishable to the reader.

**A collision fails where the cause cannot be found.** The filesystem stem for
both the BMI and the object is derived from the registered name
(`hostprogram.cppm:564`: `out.object = bdir / (stem + objExt)`). Two rule
packages sharing a name write the same object file, the second overwrites the
first, and the one surviving object is then handed to the link twice — once per
registered module. Measured with the check removed and I1 in place:

```
ld: …/target/.build-mcpp/sharedname.o: in function `initializer for module
sharedname': beta.cppm: multiple definition of `initializer for module
sharedname'; …/sharedname.o: beta.cppm: first defined here
```

One file named twice, one package named twice, and no statement anywhere that
two packages are involved or which two. **The overwrite is the silent part; the
diagnostic is merely unactionable.** An earlier draft of this section claimed
the build succeeded; it does not, and the measurement above is what corrected
it.

The collision is reachable before I1 only when two packages share a bare name
across namespaces. I1 widens it to "two packages declare the same module name",
which is why the two must ship together rather than in sequence.

The check does not extend to the index. A global uniqueness invariant over rule
package names would be invisible to `path` dependencies and to private
registries, and would add an invariant the index does not have today.

### I4. A host module must have an interface unit

Unchanged; the existing diagnostic is correct.

### I5. The leaf constraint is replaced by section 3

See section 3. The constraint as it exists today is a consequence of a missing
declaration channel, not a decision, and the specification should not codify it
as one.

Until section 3 is implemented, the diagnostic is still worth improving: a rule
that imports a third package is told only `module 'X' not found` by the
compiler, and mcpp should add the sentence that explains why — that a rule
interface is compiled alone, and that `X` must be reached through
`[build-dependencies]`.

### I6. A rule is build-time only

Unchanged. Section 3 extends the same guarantee to a rule's own dependency
subtree, which does not have it today.

### I7. No new mechanism for declaring an mcpp floor

A rule that uses a newer `mcpp::` API fails to compile on an older engine, and
the engine already recognises this: `mentions_missing_mcpp_api` matches on the
namespace `mcpp` and reports "try `mcpp self update`; this is mcpp `<ver>`"
rather than the raw compiler error. This was measured when `mcpp::warning`
entered protocol 5.

The alternative — a manifest key carrying a floor — is expensive in this
repository. An unrecognised key can make a whole manifest fail to load (#359),
and whether a new key may be published at all is decided by the index's `latest`
rather than by this repository. The residual gap, a rule that needs new engine
behaviour without using a new API, is left uncovered deliberately.

### I8. `mcpp.*` is reserved for official plugins, and using it produces a warning

Official rule packages are named `mcpp.build.<x>`. In C++ this creates no
relationship with the bundled `mcpp` module — a dot in a module name carries no
hierarchy — but to a reader the prefix reads as an endorsement, and that is a
supply-chain statement.

The rule is a warning and not an error because the engine cannot determine who
is official. A `path` dependency, a private mirror, and an internal fork are all
legitimate and all indistinguishable from the outside. The diagnostic names both
the module name and the package's fully-qualified identity, so that the reader
can see which of the two is unexpected.

---

## 3. Build-time dependencies: a declaration site with no reader

### 3.1 The finding

`buildDependencies` occurs seven times in the codebase:

| Location | What it does |
|---|---|
| `manifest/toml.cppm:1015` | parses `[build-dependencies]` |
| `manifest/toml.cppm:1692` | parses `[target.'cfg(…)'.build-dependencies]` |
| `manifest/toml.cppm:1716` | decides whether that conditional block is empty |
| `types.cppm:775`, `types.cppm:888` | the field declarations |
| `project.cppm:101` | merges it across workspace members |
| `prepare.cppm:269-270` | folds the conditional block into the manifest |

All seven are parsing, merging or propagation. **No code path reads the field to
make a decision.** It builds no edge, enters no worklist, and affects neither
resolution nor linking. An author who writes `[build-dependencies]` gets a
manifest that loads, no diagnostic, and no effect.

Two independent corroborations:

- `types.cppm:888` annotates the field `// host-side tools (M5+ behavior)`.
  M5 is future tense.
- `tests/e2e/263_lib_root_follows_the_extension.sh:122-125` records that the
  first version of that fixture placed a rule package under
  `[build-dependencies]` and "failed identically" — the fixture was wrong, not
  the resolver. Someone has already walked into this.

The documentation never promised otherwise. `docs/05-mcpp-toml.md:858` lists
`build-dependencies` among the keys a `[target.<cfg>]` block accepts, and
`:1618` mentions Cargo's `[build-dependencies]` as an analogy while describing
what `host-module = true` does. Neither states that mcpp's own section has an
effect. No package in the index uses it: a grep over all 132 descriptors and
their repositories finds the key only inside a design document.

### 3.2 Why this is the reason a rule is a leaf

There are three build-time channels, and the third is the only one a package
can use to speak about itself:

| Channel | Declared by | Product | State |
|---|---|---|---|
| `tools = [...]` | the consumer, on an edge | an executable; zero ABI contact, so the sub-build may use its own toolchain | live |
| `host-module = true` | the consumer, on an edge | a BMI and an object, compiled with `build.mcpp`'s flags; one interface unit only | live |
| `[build-dependencies]` | the package itself | — | **dead** |

Both live channels require the *consumer* to write something on an edge. A rule
package therefore cannot request anything on its own behalf. That, and not a
design decision, is why a rule package is a leaf.

Worse, what a rule package writes in `[dependencies]` today leaks. The
transitive walk pushes a dependency's own `[dependencies]` unconditionally
(`prepare.cppm:4430`), and the rule-only predicate only empties the globs of
packages every one of whose in-edges is a host-module edge
(`prepare.cppm:4816`). A rule's dependency `foo` is reached by a non-host-module
edge, keeps its globs, and is compiled and linked into the consumer's binary —
while the rule itself still cannot import it. Both halves are wrong, and the
sharper harm is that the rule's dependency versions participate in the
consumer's real resolution, so a rule can create a version conflict inside a
project that never asked for it.

**Measured, not inferred.** `tests/e2e/310_build_dependencies.sh` builds a
consumer whose only relationship to `behindrule` is that a rule it uses depends
on it, and looks for that library's symbol in the produced binary. With the old
per-edge predicate in place the symbol is there; with forward reachability it
is not. The fixture was written and seen failing before the predicate was
replaced.

### 3.3 The separation the specification states: two orthogonal axes

The section and the request answer different questions, and conflating them is
a modelling error rather than a matter of spelling.

- **The section answers whether the package itself reaches the target.**
  `[dependencies]` means it does; `[build-dependencies]` means it never does.
- **The request on the edge answers which build-time product is wanted.**
  `tools = [...]` asks for an executable; `host-module = true` asks for a
  module the build program can import.

A package may take a value on both axes at once, and protobuf is the case that
proves the axes must stay separate: a project links `libprotobuf` *and* needs
`protoc` during the build. It is written once, and it is not a build
dependency:

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

`[build-dependencies]` is for the combination the first axis cannot otherwise
express — a package whose *library* must not reach the target while its tool or
its rule is still wanted:

```toml
[build-dependencies]
# protoc is wanted; libprotobuf must not be linked into this target.
protobuf  = { version = "35.1", tools = ["protoc"] }
jsonrules = { version = "2.0",  host-module = true }
```

Two consequences follow, and both are already visible in the ecosystem.

**Features gate build-time requests without a second section.** gRPC turns its
whole codegen provision on through `[feature-deps.codegen]`, adding
`tools = ["protoc"], reexport = true` to the protobuf edge it already declares
unconditionally. The request appears when the feature is active and not
otherwise, which is what "only when it is needed" means here; no separate
declaration site is involved, and none should be introduced.

**`host-module = true` already implies the first axis in the common case.** A
package reached only by host-module edges has its globs emptied
(`prepare.cppm:4816`), and the predicate is deliberately guarded on *every*
in-edge, because a package may legitimately be both a rule and a library and
dropping its objects then would surface as an undefined reference far from the
cause. `[build-dependencies]` states the same conclusion explicitly rather than
inferring it, which is what a rule package needs in order to say anything about
its own dependencies at all.

`[dependencies]` carrying `tools` or `host-module` therefore remains valid and
unchanged; published packages depend on it. `[build-dependencies]` adds the one
thing that spelling cannot express, and gives a rule package a voice. It is not
a replacement.

### 3.4 B1 — the subtree stays out of the target

Every package reached through `[build-dependencies]`, and every package reached
transitively from one, is build-time only. It is resolved, it may be built for
the host, and it never contributes sources, objects or link flags to the
consumer's target.

This fixes the leak in 3.2 as a consequence rather than as a special case: the
current predicate asks "is every in-edge a host-module edge", which is a
property of edges, and the corrected question is "is this package reachable from
the target's own dependency roots", which is a property of the graph.

The reachability form matters because a package can be on both sides. If a
project depends on `foo` directly and some rule's build dependencies also reach
`foo`, then `foo` belongs in the target — the build-time path does not subtract
from what the project asked for. Stating the rule as exclusion ("anything under
`[build-dependencies]` is out") would get this backwards; stating it as forward
reachability gets it right without a special case.

The same package may therefore be built twice in one run, and the two builds
produce different things: a library for `--target` on the ordinary path, and
either a host executable through the tool store or a host BMI and object through
`build_host_module` on the build-time path. They do not have to agree, and they
do not interact — which is the property the tool store's design already relies
on for executables.

### 3.5 B2 — a rule may import its build dependencies

A rule may `import` any dependency it declares with `host-module = true`. The
mechanism extends the existing one rather than replacing it: instead of
compiling one interface with `build.mcpp`'s flag set, mcpp compiles N interfaces
in topological order with the same flag set. The BMI agreement argument from
section 10.5 of the 2026-08-05 document survives unchanged, because the flag set
is what that argument rests on and the flag set does not change.

`[build-dependencies]` is the correct section for a rule's own imports, and the
reason follows from the two axes in 3.3 rather than from convention. A rule
package can also be built standalone as an ordinary library, in which case
`[dependencies]` describes that library's needs. When it is consumed as a rule,
that library target is not what the consumer builds — only the interface is
compiled — so a dependency needed by the interface has no business in the axis
that decides what reaches a target. Declaring it under `[build-dependencies]`
says the one thing that is true in both modes.

What this needs: a topological order over the host-module graph, cycle
detection, and a depth bound. `tool_store::kMaxDepth` is the precedent for the
bound and for its diagnostic.

**A rule's build dependencies are not automatically importable by the
consumer.** If rule A imports rule B, B's BMI has to exist in the compile that
produces A, but the consumer's `build.mcpp` may import only A. Making B visible
as well would put entries into the consumer's module namespace that the consumer
never declared, which is the supply-chain statement `reexport = true` exists to
require. The existing propagation model answers this without a new rule:
`provisions.cppm:81` already records that a host module crosses one further edge
only on a re-exporting edge, and the fixpoint already computes `own`, `exported`
and `visible` per package. B2 extends the compile, not the visibility.

**A limit that must be stated rather than hidden.** `build_host_module` compiles
exactly one interface unit per host module — `resolve_lib_root_path` yields one
path, and the function returns one object to link. A library with implementation
units or several modules therefore still cannot be a build dependency of a rule.
Lifting that is a separate question with its own answer to give about which
objects are linked into the build program.

---

## 4. Tier 2 — shape, documented for authors

Six statements, each derived from something measured rather than from taste.
`grpcgen` is the only sample, so each trait of `grpcgen` was judged individually
as necessary or incidental; the four incidental ones are not in this list.

**This tier is the weakest part of the document, and it should be read as
provisional.** Six statements generalised from one instance are six hypotheses.
The correct test is the second rule package: whichever rule is written next
should be written without consulting this list, and the places where it
disagrees are the places where an accident of `grpcgen` was mistaken for a
necessity. Tier 1 does not have this problem, because it is derived from what
the engine executes rather than from what one author chose.

**S1. Layers must not have a cliff, and each layer must be the composition of
the one below it.** `generate_all(opt)` *is* `submit(plan_all(opt))`;
`.grpc = true` *is* `.plugins = {cpp()}`. The alternative is what the
2026-08-07 document recorded as the lesson from that batch: past two knobs the
user writes sixty lines by hand to work around the rule, and those sixty lines
drift away from the rule silently.

**S2. A rule must expose a plan/submit pair.** The escape hatch at the bottom
layer has to be able to obtain the planned edges, modify them, and hand them
back. This is the mechanism that makes S1 true; it is not a naming preference.
The verbs are.

**S3. A rule does not reproduce truth the engine already holds.** mcpp writes
every action's full argv into `build.ninja`, recoverable with
`ninja -t commands`. A second source of that truth can only drift. The half the
rule owns is which knobs produced the command, and it belongs in each edge's
`description`.

**S4. Failure and advice use different channels.** mcpp prints what it captured
from a build program only when the program exits non-zero. A failure therefore
prints a diagnostic to stderr and returns non-zero, which is what `grpcgen`
does at all nine of its diagnostic sites. A message that must be seen on a
*successful* build has to go through `mcpp::warning` (protocol 5), because
stderr on success is discarded. Choosing the wrong channel means the message is
absent from exactly the builds that needed it.

**S5. A rule package's version is its own.** See section 5.

**S6. Repository placement is a judgement, not a rule.** Living in the upstream
project's repository under `rules/` means the upstream CI exercises the rule
when upstream changes; a separate repository has to solve that another way.
The specification states the trade-off and lets the author choose.

### Testing a rule package

A rule package is consumed only by a `build.mcpp`, so compiling it proves
nothing. Its test is an example project that depends on it, builds, and asserts
on the produced artefact. The criterion sits on the artefact, not on "the rule
compiled" and not on a log line.

---

## 5. Versioning, and a migration hazard

A rule package carries its own semantic version. The version of the upstream
tool it wraps belongs in the description and in a constant inside the rule, not
in the version number.

The reason is identity. `grpcgen` is published as version `1.83.0` while its
tarball tag is `v1.83.0-4`: four different payloads have shared one version
string. mcpp's identity for an installed package is `(name, version)`, so a
repackaged rule does not trigger a reinstall, and the consumer keeps running the
old rule with no diagnostic. This repository has already paid for that shape
once, in the fourth layer of the stale-index problem, where a descriptor changed
its artefact without changing its version.

**The migration has a hazard that must be resolved before the move.** If
`grpcgen` adopts its own versioning starting below `1.83.0`, the index holds both
`1.83.0` and the new version, and a ranged dependency resolves to the highest
match — the old one wins permanently. Pointing `latest` at the new version is
not sufficient; the previous occurrence of this required withdrawing the old
version from the table.

This document does not assert the resolution rule as verified. Registry version
selection runs through the xim and xlings path rather than through
`resolver.cppm`, and the exact call site was not re-read while this was written.
**Verify it first**, then choose between withdrawing `1.83.0` and starting the
rule's own versioning above it.

---

## 6. Explicitly not done

| Not done | Reason |
|---|---|
| A manifest key carrying an mcpp floor | I7: unrecognised keys can fail a whole manifest load, and the existing `mentions_missing_mcpp_api` covers the common case |
| Generating module names as `<namespace>.<name>` | It would break `import grpcgen;`, which is published |
| A global uniqueness invariant on rule package names in the index | Invisible to `path` dependencies and private registries |
| Making `[build-dependencies]` an error in the interim | No package uses it, so wiring it is cheaper than fencing it |
| A rule importing a multi-unit library | Section 3.5: one interface unit per host module is a real limit of `build_host_module`, and lifting it is a separate design |

---

## 7. Corrections to existing documents

1. `.agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md`, section
   10.1 row 4: the `mcpp.rules.*` decision is withdrawn. It was unimplementable
   under the rule that the module name is the bare package name. It is replaced
   by `mcpp.build.*` as a **module name** prefix — the distinction the original
   decision lacked.
2. `docs/05-mcpp-toml.md:1605` and its Chinese counterpart: the paragraph
   stating that the module name is the package's `name`, and that `grpc-rules`
   is therefore illegal, is rewritten under I1 and I2.
3. `docs/05-mcpp-toml.md:858` and its Chinese counterpart: `build-dependencies`
   gains a statement of what it does, in both the top-level and the
   `[target.<cfg>]` listings.
4. `docs/07-build-mcpp.md` and its Chinese counterpart: the rule-package section
   gains the leaf-constraint replacement, the tier-2 author guidance, and the
   testing criterion.

Both languages are required by CI, as is heading-level agreement between them.

---

## 8. Verification

Each criterion below has to be observed failing before the corresponding change
is made. A replay or guard test that has never been seen red cannot distinguish
"the rule holds" from "the assertion never ran".

| # | Criterion | Notes |
|---|---|---|
| V1 | A rule whose lib root declares a module name different from its package name is importable under that declared name | Impossible today; this is I1's whole content |
| V2 | A rule package whose declared name equals its package name registers the same module name before and after the change | The ecosystem denominator here is one, so this cannot be an ecosystem sweep. It has to be a unit test over constructed packages that exercises both paths, plus the divergent-name case from I1's table on all three compiler families |
| V3 | Two host modules declaring the same module name produce an error naming both packages and both interface paths | Assert on the full output line: the two identities are the content, and a substring match on "duplicate" would survive a rewording that dropped them |
| V3b | Before the fix, that same fixture fails at the LINK with `multiple definition of 'initializer for module …'` naming one file twice | Measured. The guard has to be seen replacing that message, not merely seen green afterwards |
| V1b | The host module's BMI and object are named for the DECLARED module name, and no file is named for the package name | **The successful import does not discriminate on GCC** — its BMIs are implicit under gcm.cache, keyed by the declared name, so the registered name is never consulted. Measured: the pre-I1 engine passes the import test on Linux. The filename listing is the platform-independent criterion |
| V4 | A rule package whose `[dependencies]` name a library leaves no object of that library in the consumer's target | The leak fixture from 3.2. It must be written and seen to fail before B1 |
| V5 | A rule imports a `host-module` entry from its own `[build-dependencies]`, and the imported rule's contribution reaches the consumer's compile | Assert on an effect, as e2e 263 does, not on a log line |
| V6 | A cycle in the host-module graph is reported as a cycle, naming the packages on it | Not as a depth-limit message, which answers a different question |
| V7 | A non-official package declaring an `mcpp.*` module name warns, and the build still succeeds | Both halves; a test that only checks the warning cannot tell a warning from an error |
| V8 | The second build of an unchanged project prints none of the above | The whole-project fast path (`try_fast_build`) never reaches `build.mcpp`, so these tests must touch a source first or they measure the fast path |

V8 is stated because this repository has read that measurement wrongly twice
before, once for the directive cache and once for the advisory channel.

---

## 9. Open questions

1. **Registry version selection.** Section 5: confirm how a ranged dependency
   chooses among available versions before planning the `grpcgen` migration.
2. **Which official rules come first.** This document specifies the form, not
   the batch. Three of the four action roles — `check`, `object`, `artifact` —
   have no rule package in the ecosystem, and `role = check`'s "parallel by
   default" decision has never been exercised by a real consumer. That is an
   argument for what the first official rules should be, and it is deliberately
   left to the next round.
3. **Linking objects for multi-unit build dependencies.** Section 3.5's stated
   limit. The question is which objects join the build program's link, and it
   has to be answered before a rule can depend on an ordinary library.

---

## 10. The framework in one view

Four layers. The only thing an author writes is L1, and what L1 emits is either
a patch to L0's answers or a node in L2's graph.

```
L0  mcpp.toml                     declaration; statically parsable
      [dependencies]              reaches the target
      [build-dependencies]        serves the build only, never the target
        per-edge requests:  tools = [...]        -> a host executable
                            host-module = true   -> an importable host module
      ^ programs may not write here: lockfile, LSP and audit depend on it

L1  build.mcpp                    configuration; ONE hook, runs once
      import mcpp;                the typed surface that evolves
      import <rule>;              a rule package, compiled with this program
      emits either
        a patch      cxxflag / define / generated / link_lib / runner / warning
        or a node    mcpp::action{ id, role, inputs, outputs, command }

L2  ninja                         the graph; incremental, parallel, attributable
      compile | link | action(source | check | object | artifact)

L3  mcpp pack                     distribution; decoupled from the build
```

Two directional rules hold the shape together. **L1 writes downwards only**: it
declares nodes in L2 and never edits L0's dependency graph. And **a role is not
a mechanism**: all four roles are the same edge wired to a different place, so
"post-build processing" is an action whose *inputs* are link outputs rather than
a new lifecycle hook — which is why it cannot double-apply itself.

A rule package is an ordinary mcpp package whose lib root is a module. Marked
`host-module = true`, its interface is compiled in the same command, with the
same flags, as the `build.mcpp` that imports it. That is what makes the BMI
usable there: agreement on standard, dialect and compiler identity is a
structural fact rather than a property somebody has to verify.

---

## 11. Scenarios

Every fragment below uses only API that exists today, except where marked
**(B2)**. The forms are taken from `tests/e2e/188_build_actions.sh` and
`tests/e2e/189_host_module_rules.sh`.

### 11.1 `role = "source"` — generated code enters the compile set

```cpp
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/gen.cpp";
    mcpp::action a;
    a.id = "generate"; a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg((root + "/data/value.txt").c_str())
     .arg(out.c_str())
     .input((root + "/data/value.txt").c_str())
     .output(out.c_str())
     .submit();
}
```

Editing `data/value.txt` re-runs this one edge. Editing something unrelated
re-runs nothing. Writing the same file eagerly from `build.mcpp` instead gives
up all three of incrementality, parallelism and per-edge attribution.

For a generated **module interface**, the interface has to be declared as well,
because the module graph is fixed during prepare:

```cpp
a.output(gen.c_str()).provides("my.generated").imports("std").submit();
```

mcpp seeds a placeholder carrying exactly that declaration, and the compiler's
P1689 output checks the claim at build time.

### 11.2 `role = "check"` — analysis that runs beside the compile

```cpp
mcpp::action c;
c.id = "lint"; c.role = "check";
c.arg((root + "/check.sh").c_str()).arg("${mcpp.out_dir}/lint.stamp")
 .output("${mcpp.out_dir}/lint.stamp")
 .submit();
```

The output is a stamp, and the command must create it. A failing check fails the
build; `blocking = true` additionally gates compilation on it, which is worth
paying only when a failure means the compile was wasted anyway.

**This is where a rule package earns its keep.** `clang-tidy` does not write a
stamp, so every project that wants this writes the same wrapper script.
`${mcpp.compile_db}` gives the `-p` argument clang-tidy wants. A `tidy` rule
package would own the wrapper, the stamp convention and the file set, and no
consumer would write a shell script again. No such package exists today, and
`role = "check"` has no ecosystem consumer at all — so the "parallel by default"
decision has never been exercised by a real user.

### 11.3 `role = "object"` — an object joins the link

```cpp
mcpp::action o;
o.id = "blob"; o.role = "object";
o.arg((root + "/mkobj.sh").c_str()).arg((root + "/blob.cpp").c_str())
 .arg((out + "/blob.o").c_str())
 .input((root + "/blob.cpp").c_str())
 .output((out + "/blob.o").c_str())
 .submit();
```

Prefer omitting `.target(...)`: with no target the outputs attach to every image
the package produces in this build, **including test binaries**. Leaving those
out makes `mcpp build` succeed while `mcpp test` dies with an undefined symbol
on the very symbol the action exists to provide.

Naming a pre-built object in `[build].ldflags` also reaches the linker and
should not be used for anything the build produces: ldflags is a flat string in
the link command, not a file in the graph, so editing it reports
`ninja: no work to do`.

### 11.4 `role = "artifact"` — post-processing, without a post hook

```cpp
mcpp::action p;
p.id = "package"; p.role = "artifact";
p.arg((root + "/pack.sh").c_str()).arg("${mcpp.target_file:app}")
 .arg("${mcpp.out_dir}/app.pack")
 .input("${mcpp.target_file:app}")
 .output("${mcpp.out_dir}/app.pack")
 .submit();
```

Its inputs are link outputs, so ninja's own file dependencies do the sequencing.
`${mcpp.target_file:NAME}` resolves to a build-dir-relative path, because ninja
identifies a file by the string the edge declared and an absolute path to the
same bytes is a different node.

### 11.5 A rule package, as it can be written today

```toml
# rules/mcpp.toml
[package]
name    = "tidy"
version = "0.1.0"
[targets.tidy]
kind = "lib"
```

```cpp
// rules/src/tidy.cppm
export module tidy;          // under I1 this name is the contract, not the package name
import std;
import mcpp;

export namespace tidy {
inline bool check(std::span<const std::string> files) {
    const char* exe = mcpp::dep_bin("llvm", "clang-tidy");
    if (!exe || !*exe) {
        std::println(std::cerr,
            "tidy: no clang-tidy. Declare it on the edge that provides it:\n"
            "  llvm = {{ version = \"...\", tools = [\"clang-tidy\"] }}");
        return false;                       // stderr + non-zero exit: mcpp prints it
    }
    for (auto const& f : files) { /* one action per file */ }
    return true;
}
}
```

```toml
# the consumer
[dependencies]
tidy = { version = "0.1.0", host-module = true }
```

```cpp
// the consumer's build.mcpp
import mcpp;
import tidy;
int main() { return tidy::check_all() ? 0 : 1; }
```

The rule is versioned, testable and distributable through the package manager
that already exists, and it is written in C++. No second language is introduced,
which is the premise `build.mcpp` exists to defend.

### 11.6 A rule that depends on another rule **(B2)**

```toml
# rules/mcpp.toml
[build-dependencies]
globbing = { version = "1.0", host-module = true }
```

```cpp
// rules/src/tidy.cppm
export module tidy;
import std;
import mcpp;
import globbing;          // impossible today: the rule interface is compiled alone
```

`globbing` is compiled before `tidy`, in the same command and with the same
flags, so BMI agreement stays structural. It is **not** visible to the
consumer's `build.mcpp`: crossing one further edge needs `reexport = true`,
which is the same supply-chain statement the existing provision model already
requires.

### 11.7 A package that is both a library and a tool provider

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

One declaration, two answers. The project links `libprotobuf`, and
`mcpp::dep_bin("protobuf", "protoc")` returns a host executable built through
the tool store. The section decides whether the *package* reaches the target;
the request decides which *build-time product* is wanted. Writing protobuf
twice, or putting it under `[build-dependencies]`, would say something false —
namely that the library is not linked.

Use `[build-dependencies]` for the combination the first axis cannot express:

```toml
[build-dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }   # protoc yes, libprotobuf no
```

### 11.8 A library standing up the whole toolchain for its user

```toml
# inside the grpc package's manifest
[feature-deps.codegen]
"compat.protobuf" = { version = "35.1",   tools = ["protoc"],          reexport = true }
grpc-plugin       = { version = "1.83.0", tools = ["grpc_cpp_plugin"], reexport = true }
grpcgen           = { version = "1.83.0", host-module = true,          reexport = true }
```

```toml
# what the user writes
[dependencies]
grpc = { version = "1.83.0", features = ["codegen"] }
```

```cpp
// and their whole build.mcpp
import mcpp;
import grpcgen;
int main() { return grpcgen::generate_all() ? 0 : 1; }
```

A feature turns the build-time requests on only when they are wanted, which is
why no separate declaration site is needed for "sometimes". `reexport` is off by
default and is deliberately not carried by `visibility` — handing something to
your consumers is a statement that has to be written down.

---

## 12. Splitting mcpp's own source tree into `modules/`

The proposal is to give mcpp the layout xlings already adopted: independent
packages under `modules/`, each with its own `mcpp.toml`, consumed from `src/`
by path, so that `platform`, `toml`, `json` and their neighbours become
maintainable units — and, eventually, units the ecosystem can use.

The two pieces of work belong in one document because one of them gates the
other, and because the measurement below says the split is smaller than it
looks.

### 12.1 What mcpp is today, measured

158 translation units, 68,738 lines, **157 declared modules, one package, one
target**. Grouping the modules by directory and computing strongly connected
components over the group graph:

```
group-level SCCs:   ONE component of size 17
                    bmi_cache build config diag fallback fetcher freestanding
                    home lockfile manifest modgraph pack platform pm project
                    toolchain ui

true leaves (6):    dyndep  libs  log  source_kind  version  version_req
```

Seventeen of the twenty-one groups are mutually entangled. Taken at face value
this says the tree cannot be split at all.

### 12.2 The entanglement is thin, and it is a placement problem

The module-level graph is necessarily acyclic — C++ modules cannot import in a
cycle — so every group-level cycle is produced by a countable set of specific
back-edges. Counting them changes the conclusion:

| Candidate | Back-edges into non-leaf groups | Where they are |
|---|---|---|
| `platform` | **5** | 3 in `platform/xlings/xlings.cppm` → `pm`; 1 in `platform/xlings/runtime_selection.cppm` → `manifest`; 1 in `platform/runtime_binding.cppm` → `config` |
| `manifest` | 12 | 8 → `pm`, and all eight land on `pm.dep_spec`, `pm.index_spec`, `pm.compat`, `pm.dependency_selector`; 4 → `platform` |
| `toolchain` | 35 | **25 of them → `platform`**, which is downward once `platform` is a package |
| `modgraph` | 6 | 3 → `toolchain`, 2 → `manifest`, 1 → `platform`; all downward under the layering below |
| `pm` | 42 | genuinely central; not a candidate |

Two diagnoses follow, and both are about naming rather than architecture.

**`src/platform/xlings/` is not platform.** Three files, 2,206 lines, and they
are the xlings integration, which by its nature knows about packages, indexes
and manifests. It is a different layer wearing platform's name. Excluding it,
`platform` is 20 files and 4,852 lines with exactly **one** remaining back-edge
(`runtime_binding.cppm` → `config`). Extracting platform is a move, not an
untangling.

**The four `pm` modules that `manifest` imports are vocabulary, not
machinery.** `dep_spec` is 129 lines with zero mcpp imports; `index_spec` is 59
lines with zero; `dependency_selector` (222) imports only `dep_spec`; `compat`
(272) imports only `compat.legacy` and `dep_spec`. Roughly 680 lines that sit
*above* manifest by directory and *below* it by nature. Moving them under
manifest removes all eight edges at once.

### 12.3 The layering that falls out of the measurement

```
modules/     independent packages, each with its own mcpp.toml, used by path
  M0   log, version, version_req, source_kind, dyndep, libs(json)
         6 modules, zero mcpp imports; extractable with no edge cut at all
  M1   platform, minus platform/xlings/
         20 files, 4,852 lines; one edge to cut
  M2   vocabulary: dep_spec, index_spec, compat, dependency_selector
         ~680 lines; removes manifest's dependency on pm
  M3   manifest
         4 files, 5,560 lines
src/         mcpp itself, including main.cpp and the binary target;
             everything not yet separated, and shrinking it is the direction
```

**Two directories, not three.** xlings additionally separates `apps/` from
`modules/`, and that separation earns its place there because xlings produces
two binaries — `xlings` and `xlings-gui` — so "what becomes a binary" is a
category with more than one member. mcpp produces one binary, inferred from
`src/main.cpp`. An `apps/` holding a single member would be a directory with a
name and no distinction behind it, which is the kind of structure that has to be
explained every time someone reads it. The binary stays in `src/`.

The rule mcpp adopts is therefore the one half that applies: **`modules/` holds
what gets linked into the binary; `src/` is what has not been separated yet.**

### 12.4 The mechanism, as xlings actually uses it

Two declarations are required, and this is easy to get wrong because either one
alone looks sufficient:

```toml
[workspace]
members = ["modules/log", "modules/platform", ...]    # membership: -p, mcpp test

[dependencies]
platform = { path = "modules/platform" }              # the import edge
```

Workspace membership does not make a module importable; the path dependency
does. xlings also recorded the constraint it hit: **dependency names resolve in
one flat namespace**, so a member that wraps a registry package cannot share
that package's bare name — their `tinyhttps` wrapper had to be called `xhttp`.
mcpp's split will meet the same wall wherever a module's natural name collides
with an index entry.

Module names are not affected. An ordinary library package's module names come
from its source (`prepare.cppm:3924`), so `modules/platform` can be the package
`platform` while continuing to declare `export module mcpp.platform;` and every
one of the 35 importers stays unchanged. **The split does not rename modules.**

### 12.5 Where this meets the rule-package work

The connection is real, and it runs in one direction that has to be stated
precisely rather than assumed.

**A `modules/*` package cannot be used by a rule until section 3.5's limit is
lifted.** `build_host_module` compiles exactly one interface unit per host
module, and `modules/platform` is twenty files. So "the ecosystem can now use
mcpp's own platform layer from a build rule" is **not** a consequence of the
split; it is a consequence of the split *plus* the multi-unit host-module work
that section 9's third open question describes. Publishing `modules/` is
necessary and not sufficient.

Two smaller couplings:

- The two axes of section 3.3 answer where each module goes without a new rule.
  `modules/*` are `[dependencies]`: they are linked into the mcpp binary, so
  they reach the target. They are not build dependencies.
- I1 does not gate the split, because ordinary library packages already have
  authored module names. I1 gates only the case where a `modules/*` package is
  *also* offered as a rule, where today the module name would be forced to equal
  the package name.

### 12.6 What must be measured before committing to the split

These are risks with instruments, not objections. Each has a way to be answered
and none should be argued from first principles.

**Build latency across a package boundary.** mcpp's build is critical-path
bound: added cores do not help, so any new serialisation is paid in full wall
clock. mcpp's own manifest sets `bmi_schedule = "on"` precisely so that
importers start when a BMI is published rather than when the compiler exits.
Whether that early start survives a package boundary — whether the root's
compiles wait for `modules/platform`'s BMIs or for its archive — decides whether
this split is free or expensive. `bench/` is the instrument. Measure before
moving a single file.

**Dependency build caching.** A dependency that reports a cache hit and then
recompiles everything would turn one package into ten times the work.
`fill_package_config` (`cache_key.cppm:337`) reads each package's own
`buildConfig` into the key, so the per-package axis exists; confirm by
measurement that a hit is a hit.

**Bootstrap.** mcpp is self-hosted and its bootstrap mcpp is pinned separately
from the release, so the pinned version must already understand workspaces plus
path dependencies. xlings demonstrates the feature works; the pin is the thing
to check, not the feature.

**Order.** M0 first: six modules, zero edges to cut, and it exercises the whole
mechanism — workspace member, path dependency, bootstrap, cache, CI — against
the smallest possible blast radius. If M0 measures badly, nothing further should
proceed.
