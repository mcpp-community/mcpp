---
subject: triage
status: landed
---

# What a framework and its ecosystem library still hit in the engine: the ten items of #630, read against the code

**Status:** landed on 2026-09-13 as mcpp 2026.9.13.2 (mcpp-community/mcpp#631),
`llvm.libcxx` 22.1.8.1 (new repository `mcpplibs/libcxx`),
`llvm.compiler-rt-builtins` 22.1.8.5 (mcpplibs/compiler-rt-builtins#1, #2),
mcpp-index #408 and #409, and a `mcpp:plugins` change that follows the
release. Every statement about the engine was read at `b63dc4e5` (mcpp
2026.9.13.1), the version the issue measured against. §0 classifies the ten
items; §1 states the rules the changes share; §2 to §9 take the items one at
a time with the code, the classification, the shape and the criterion in both
directions; §10 is the order and what the ecosystem side deletes; §12 is the
task list; §13 records what landed and where each claim was measured.

## 0. The ledger

#630 lists eight items and restates two from #622. The issue's own framing is
correct in one respect that governs everything below: nothing in it is
framework-specific and nothing blocks the work, so each item is judged by
whether it is a property of the engine that any second framework would hit,
not by whether it inconveniences this one.

| # | the issue says | measured | classification |
|---|---|---|---|
| 1 | two `git` revisions of one identity: the root wins silently | no field of a `git` or `path` declaration is ever compared; the record kept is the one dequeued first, and the root is dequeued first because it is seeded first (`prepare.cppm:6407-6432`, `6764-6774`) | engine defect, general. The rule the issue infers is an accident of queue order |
| 2 | `path` in the root against `git` in a dependency is a hard error | confirmed at `prepare.cppm:6480-6486`; no override table exists in the manifest | engine gap, general. Decided together with 1 |
| 3a | Mach-O programs are not staged, so a bundle carries no deployed files | the refusal at `pack.cppm:1240` precedes the first staging step at `pack.cppm:1279`; the pipeline already carries the failure to the dispatched format with an empty stage directory (`pipeline.cppm:352-365`, `408-410`) | engine defect: an order of operations. Declared files need no loader |
| 3b | the Mach-O closure needs a reader | `binfmt.cppm` has a static reader for ELF (`216-310`) and for PE (`320-415`); Mach-O is identified and refused (`472-497`). The ELF pack path still runs the loader (`pack.cppm:1376`) although the reader exists | engine gap, general. One rule for three formats |
| 4 | iOS links the payload's libc++ headers against the SDK's libc++, and no compiler-rt | by construction: the compile side emits `-nostdinc++ -isystem <payload>/include/c++/v1` (`hostflags.cppm:341-343`, `linkmodel.cppm:178`) while the link side chooses the SDK's `-lc++` (`distribution.cppm:485-530`). The 2026-09-11 record's table says "the payload's libc++" for the iOS rows; `distribution.cppm` says the opposite, and the code is right | engine defect: the two halves of one seam are chosen in two files. Resolved by a runtime package for Apple's other platforms, so that the iOS rows take the macOS rows' contract (§5) |
| 5 | `min_api_level` is honoured and reported as unsupported | confirmed (`toml.cppm:2635`, `2744-2760`); it is the only parsed non-table key absent from the known list, the drift dates from #610, and `--strict` promotes the warning to an error (`prepare.cppm:2058-2061`) | engine defect. Not cosmetic under `--strict` |
| 6 | a cached host tool outlives its source | the store key holds package, version, host, compiler, profile, features and `name@version` of the closure, and no source content (`tool_store.cppm:77-97`, `prepare.cppm:8721-8745`). The object cache excludes `path` and `git` packages by rule (`cache_key.cppm:44-49`); the tool store has no such rule. Measured on 2026-09-08 with `examples/12` | engine defect, known and unrecorded until now |
| 7 | `mcpp emit xpkg` reads the top-level `[xlings.workspace]` only | confirmed, and already reported by the `publish/target-axis-tools` warning (`publisher.cppm:215-233`). At build time the consumer's mcpp folds a dependency's target-axis tools and provisions them (`prepare.cppm:265-294`, `7009-7012`, `4541-4560`), so the consequence is a descriptor that under-states install-time dependencies, not a link failure | half usage (the warning names the workaround), half engine: a selector that names only an operating system is a platform |
| A8 | a dependency's `[xlings.workspace]` should reach the consumer's build program | decided in the 2026-09-12 record §2.8: a host module's declarations reach every build program compiled from it (`fillXpkgDirs`, `prepare.cppm:5680-5751`); an ordinary library's do not, by design | usage. Declined again, with the reason restated in §8 |
| A9 | one artifact from several targets | the library route already takes several triples (`build_and_pack_library`, `library_pipeline.cppm:113`); the program route refuses more than one (`cmd_publish.cppm:124-129`). On Android an `app` is a shared object | engine gap, small. The universal APK is the library route applied to an app |

The issue's "what is not here" list is accepted as written. One addition:
HuxerUI's builtins lookup falls through to Xcode's `libclang_rt` through
`xcrun --find clang` when the payload lacks the archive. That is the host
fallthrough the recorded host-surface rule forbids, and it exists only because
the engine is silent where it should refuse (§5.3). It is listed in §10 as a
workaround the ecosystem deletes.

## 1. The rules the proposals share

Six items reduce to four rules, three of which the engine already applies
elsewhere. Naming them is what keeps the ten fixes from being ten policies.

**Rule A. One identity, one resolution, said aloud.** The tool plane already
has this: `addrset::unify` (`address_set.cppm:124-177`) lets the declaration
nearest the artifact win, checks every other declaration against the winner,
reports a difference as `xlings/version-override` and refuses a violated
requirement. The dependency graph has the refusal for a violated SemVer
constraint and nothing else. Items 1 and 2 give it the rest of the same
rule. The precedent for "the root alone may decide a whole-graph property" is
`DependencySpec::linkage`, honoured only on the root's edges because "a form
is a whole-image decision" (`dep_spec.cppm`).

**Rule B. A closure is read, not run.** PE already does this
(`pe_closure` through `needed_names`). Reading is the only mechanism that
works for a format whose loader ignores the trace variable (Mach-O), for a
host that cannot execute the artifact (a Linux host packing a macOS program;
a Windows host packing an ELF, `pack.cppm:1257`), and for an operating system
whose libraries are not on disk at all (macOS 11 and later keep them in the
shared cache). Items 3a and 3b apply it; the ELF path is the third
beneficiary and is not changed in this batch.

**Rule C. The two halves of a seam are chosen in one place.** The libc++ a
translation unit is compiled against and the libc++ it is linked against are
one decision. Today the compile half is made in `hostflags.cppm` from a
question about the C ABI and the link half in `distribution.cppm` from a
question about the target's format and contract. Item 4 makes the two halves
agree by supplying the runtime the link side lacked, so that the contract the
macOS rows already have applies unchanged; the header set is then not a
second decision at all.

**Rule D. A list that a check reads must be the list the parser reads.** The
`[target.<triple>]` sweep keeps its own array of known keys next to a parser
that finds keys by name; the two drifted in #610. Item 5 is one line today and
one table tomorrow.

Two further rules bind single items. **A cache key holds everything that can
change the bytes** (item 6; `cache_key.cppm` states it and the tool store does
not follow it). **A selector that names only an operating system is a
platform** (item 7; the descriptor's three blocks are keyed by exactly that).

## 2. Items 1 and 2: one identity, two declarations

### 2.1 What the code does

The worklist is seeded from the root's `[dependencies]`, `[dev-dependencies]`
and `[build-dependencies]` before any transitive edge is pushed
(`prepare.cppm:6407-6432`), and processed first in, first out. When an
identity is already in `resolved`:

- a different *kind* (`path` / `git` / `version`) is refused with "requested
  as both a … dep (by …) and a … dep (by …). Pick one" (`6480-6486`);
- two `version` declarations are AND-merged through `try_merge_semver`
  (`6488`; `resolver.cppm:257-288`), and an irreconcilable pair is refused;
- two `git` or two `path` declarations fall through to the edge-recording
  branch under the comment "same version (or compatible path/git)"
  (`6764-6774`). `ResolvedRecord` (`4612-4626`) holds `version`,
  `constraint`, `requestedBy`, `source` and `depIndex`; it holds no path and
  no `gitRev`/`gitRefKind`, so there is nothing to compare.

Therefore the rule is not "the root wins". It is "the first requester
dequeued wins", which is the root when the root declares the identity and an
arbitrary dependency otherwise. `mcpp.lock` records `spec.gitRev`, the literal
the root wrote, as `version` (`12112-12129`), and records root-declared git
dependencies only; the sha in the issue's lock excerpt is there because the
root wrote a sha. `docs/05` promises to answer "what happens when two of them
disagree" and answers it for SemVer only; no test exercises the "Pick one"
refusal.

### 2.2 Decision

Rule A, applied to the dependency graph, with the root's privilege bounded
the way `linkage` bounds it:

| first declaration | second declaration | today | proposed |
|---|---|---|---|
| any | same kind, same reference | edge only | unchanged |
| `version` | `version`, different constraint | SemVer merge, refuse if empty | unchanged |
| `git` | `git`, different `rev`/`tag`/`branch` | silent, first dequeued wins | the root's declaration wins if the root is a requester, else the first dequeued; `dependency/source-override` warning naming both requesters and both references |
| `path` | `path`, different directory | silent | same as the row above |
| root `path` or `git` | dependency `git` or `version` | refused | the root's wins, same warning; a dependency's `version` constraint is checked against the `[package] version` of the root's checkout and refused if violated |
| dependency `path`/`git` | dependency of another kind | refused | unchanged: a dependency may not change another dependency's source kind. The hint gains one sentence: "declare `<identity>` in the root to settle it" |

The check in the fifth row is what makes the issue's "where this is going"
paragraph unnecessary as a separate feature: once the framework is on the
index with SemVer tags, a library's `^0.3` is a requirement and the root's pin
(a version, a tag, a path) is checked against it exactly as `unify`'s
`Violated` verdict checks a tool pin against a floor. There is no solver and
no second policy; the checkout's manifest already states its version.

A `[patch]` table is not proposed. The root-wins rule is the general form for
a graph whose root is the artifact; Cargo needs `[patch]` because its
resolver may select several versions of one crate and the root's own edge is
not privileged. mcpp has one package per identity by decision (the 2026-09-06
record) and the root's edge is already privileged for `linkage`.

### 2.3 Shape

- `ResolvedRecord` gains `sourceRef` (for `git`: `<url>#<kind>=<ref>`; for
  `path`: the canonical absolute directory) and `fromRoot` (the requester's
  `consumerDepIndex == kMainConsumer`).
- At the `resolved.find(key)` hit, the six rows above are one `switch` over
  (kind of the record, kind of the item, `fromRoot` of each). A root override
  re-points `depIndex` when the record came from a dependency and the root
  arrives later; this cannot happen under FIFO seeding but is written so that
  the rule does not depend on the queue order that produced today's accident.
- Warnings go through `mcpp::diag::warning("dependency/source-override", …)`
  with the same three-part shape the tool plane uses: what was declared by
  whom, which won and why, how to take the other.
- `docs/05` gains the table above under a heading that answers the sentence
  its own preamble promises; the zh mirror gains the same table.

### 2.4 Criteria

One e2e fixture with an application and two libraries, all `path`-local git
repositories so the test needs no network:

1. root `git rev=A`, library `git rev=B`: exit 0; the warning names the root,
   the library, `A` and `B`; `mcpp.lock` records `A`; the library's object
   is compiled against `A` (a marker header present only at `A`).
2. root `path`, library `git`: exit 0 with the same warning; the compiled
   library sees the working tree's marker, not the committed one.
3. the root's declaration removed, two libraries at `B` and `C`: exit 0; the
   warning names the two libraries; the winner is stated in the message.
4. two libraries, one `path` one `git`: refused with today's message plus
   the new hint.
5. root pins a checkout whose `[package] version` is `0.2.0`; a library
   requires `>=0.3`: refused, naming both.
6. Negative direction: the same reference declared twice produces no
   warning, and `mcpp build --strict` exits 0. Without this row the fixture
   passes on an implementation that warns unconditionally.

## 3. Item 3a: stage what is declared before walking what is discovered

### 3.1 What the code does

`pack::run` refuses a Mach-O program on every host at `pack.cppm:1240-1255`;
the staging directory is created at `1279`, the program copied at
`1286-1295`, and `stage_runtime_files` (the `mcpp::deploy` and
`[runtime] deploy` entries, collected into `plan.runtimeDeployFiles`) runs at
`1297`. The pipeline already tolerates the refusal for a dispatched format:
it prints a warning, leaves `pack_stage_dir` empty, and lets
`${mcpp.stage_dir}` refuse at expansion with the reason attached
(`pipeline.cppm:352-365`, `408-410`). The comment there records the decision:
"staging is a service to the provider, not a precondition for dispatch".

So `dist-apple` reaches its action and finds nothing to place, which is what
the issue measured. The deployed files are declared, not discovered; the
loader is needed for none of them.

### 3.2 Decision

The staged tree is produced in every case and the closure is one step within
it whose outcome is recorded:

1. wipe and create the staging root;
2. copy the program (or the shared object, on the Android rows);
3. stage the declared files (`stage_runtime_files`);
4. resolve the closure by the format's mechanism; a format whose mechanism is
   unavailable returns a reason instead of a list;
5. the format-specific tail (rpath rewrite, strip, debug split) runs only when
   step 4 produced a list.

For `--format tar` and `--format dir` an unavailable closure remains the
command failing, as today: the archive is the closure. For a dispatched
format the tree from steps 1 to 3 is handed over, `pack_stage_dir` is set,
and the stage manifest gains `closure = "walked" | "not-walked"` with the
reason. The warning the pipeline prints today changes from "no staged tree"
to "staged without its dependency closure: <reason>", and a provider that
needs the closure reads the manifest and says so.

This is the smallest change that gives `dist-apple` a resource destination to
fill (`Contents/Resources/` on macOS, the bundle root on iOS; that placement
is the member's, as the 2026-09-12 record decided for `dist-apk`). It also
removes the platform-specific refusal from the engine's staging path: the
refusal becomes step 4's reason for one format on one host.

### 3.3 Criteria

- On a macOS runner, `mcpp pack --format app` of a program with one
  `mcpp::deploy` entry: the staged tree contains `bin/<program>` and the
  deployed file; the stage manifest says `not-walked`; the `.app` the member
  produces contains the deployed file at the member's destination.
- On a Linux host, `mcpp pack --format dir` of an ELF program: byte-identical
  tree to today's (the reorder must not change the ELF product).
- Negative direction: `mcpp pack --format tar` of a Mach-O program still
  exits non-zero with the reason; without this row the change could be read
  as "tar of an incomplete closure succeeds".

## 4. Item 3b: a Mach-O reader, and the rule it completes

### 4.1 What exists

`binfmt.cppm` reads `DT_NEEDED` from an ELF by walking `PT_DYNAMIC` and
`DT_STRTAB` (`216-310`) and the import directories of a PE (`320-415`);
`needed_names` dispatches on the magic (`492`) and returns "not implemented"
for Mach-O (`494-497`). The pack path uses the PE reader for PE closures
(`pack.cppm:908`) and `ldd_parse`, which runs the program under
`LD_TRACE_LOADED_OBJECTS=1` (`548-561`), for ELF (`1376`). Mach-O is
identified in all five magics including the fat wrapper (`472-481`).

### 4.2 Decision

Complete `needed_names` for Mach-O and make the closure walk a function of
the format's reader (Rule B):

- **Reading.** For a thin file, walk the load commands; `LC_LOAD_DYLIB`,
  `LC_LOAD_WEAK_DYLIB` and `LC_REEXPORT_DYLIB` contribute names,
  `LC_RPATH` contributes search entries. For a fat file, select the slice
  whose `cputype`/`cpusubtype` match the resolved triple and read that; a
  slice for another architecture is not the artifact being packed.
- **Resolving.** `@executable_path` and `@loader_path` are resolved against
  the staged program; `@rpath` against each `LC_RPATH` entry in order, with
  the same two prefixes substituted. An absolute name under `/usr/lib/` or
  `/System/Library/` is the operating system's and is never bundled: the
  Mach-O row of `is_system_lib`, alongside the glibc row that exists for
  ELF (`pack.cppm:529-533`). The result is the `ResolvedDep` list the ELF
  branch already consumes.
- **Bundling.** The dylibs that resolve to a payload or a build-tree path are
  copied beside the program, and the program's `LC_RPATH` must then name the
  bundled directory. This is the Mach-O counterpart of the `$ORIGIN` rewrite
  and it needs a load-command editor (`LC_RPATH` entries live inside the
  header's padding, and a longer entry may not fit). The reader and the
  bundle-set computation land first and are measured on a macOS runner; the
  rewrite is designed after that measurement, not before it, since the
  alternative (linking with `-rpath @executable_path/../Frameworks` at build
  time so that no rewrite is needed) may make the editor unnecessary for the
  artefacts mcpp produces.

The ELF path is not moved off `ldd_parse` in this batch. It is noted as the
third beneficiary: the reader exists, and reading is what would let a Linux
host pack for another glibc or a Windows host pack an ELF.

**What landed.** The reader (`macho_needed`, thin and fat, both byte
orders), the Mach-O row of `is_system_lib`, the `@rpath` resolver
(`resolve_macho_names`) and `needed_names`'s dispatch to them, with unit
tests over generated fixtures. The closure step of `pack::run` still reports
a Mach-O program as `not-walked`: wiring the reader into that step without
bundling would name a closure the tree does not carry, and bundling needs the
`LC_RPATH` decision the measurement above is for. The dispatched format
therefore receives the program and the declared files (item 3a) and a
manifest that says so, which is what `dist-apple` needs today.

### 4.3 Criteria

- Unit: a checked-in thin arm64 Mach-O and a fat (`x86_64` + `arm64`) Mach-O
  with known `LC_LOAD_DYLIB` and `LC_RPATH` lists; `needed_names` returns
  them in order; the wrong slice is never read (a name present only in the
  other slice is absent).
- On a macOS runner: a program linked against the payload's `libc++.dylib`
  through `@rpath` resolves it to the payload path and lists
  `/usr/lib/libSystem.B.dylib` as the system's; the staged tree contains the
  former and not the latter.
- Negative direction: a program with no `LC_RPATH` and an `@rpath` name
  resolves to "unresolved: <name>" and the pack reports it, rather than
  silently skipping the entry.

## 5. Item 4: one libc++ for the iOS rows, `import std` kept, and a refusal where the driver is silent

### 5.1 What the code does

On the iOS rows `cAbiPrebuilt` is true (the C library is the SDK's), so
`graphSuppliesTarget` is false (`hostflags.cppm:341`) and the compile side
emits the payload's header set: `--no-default-config -nostdinc++
-isystem <payload>/include/c++/v1` (`linkmodel.cppm:178`), followed by
`-isysroot <sdk>` (`hostflags.cppm:402-403`). The std module is precompiled
from the payload's `share/libc++/v1/std.cppm` against the same headers with
`-isysroot <sdk>` added (`prepare.cppm:3768-3790`). The link side, in
`distribution.cppm:485-530`, chooses `HostCoupled` with ` -lc++` for every
Apple cross target, under a comment that states the reason with precision:
"iOS takes its C++ runtime from the SDK, and has no other option": the
payload's static archives are built for macOS and ld64 refuses them in an
iOS link, and the payload's `libc++.dylib` is not on a device.

So every translation unit, and the std module, is compiled against libc++
22's headers and linked against the libc++ the SDK's `libc++.tbd` describes.
The mismatch is silent until an inline function in the newer headers
references a symbol the older dylib does not export, which is the issue's
`__hash_memory` and `__atomic_notify_all_global_table`. The 2026-09-11
record's table for item D lists "the C++ runtime: the payload's libc++, as
on every other Apple row"; that statement was superseded by
`distribution.cppm` during the same batch and the record was not corrected.
The row's workaround (pin `llvm@20.1.7`) works because libc++ 20's headers
happen to reference nothing the iOS 18 SDK lacks; it is a coincidence about
two version numbers, not a fix.

The same class exists on macOS: the `HostCoupled` fallback (no deployment
floor resolved, or a payload without archives; `distribution.cppm:542-551`)
links `/usr/lib/libc++` under the payload's headers.

**Measured on a macOS runner (Xcode 16.4, run 34757885971, 2026-09-13),
while this revision was written.** The payload's `lib/clang/22/lib/darwin/`
holds `libclang_rt.osx.a` and the macOS sanitizer runtimes and no `ios` or
`iossim` archive; `clang -###` for `arm64-apple-ios18.0-simulator` adds no
`libclang_rt` at all. The payload's libc++ is `_LIBCPP_VERSION 220108`; the
macOS 15.5 and iOS 18.5 SDKs carry `190102`, their `libc++.tbd` exports
neither `__hash_memory` nor `__atomic_notify_all_global_table`, and neither
SDK ships `usr/share/libc++/v1` (no module sources). A program using
`std::unordered_map<std::string, int>` and `std::atomic<int>::notify_all`
failed to link in all three arrangements tried: payload headers over the
SDK's dylib (mcpp's shape today), SDK headers over the SDK's dylib, and
payload headers over the payload's macOS archives. The issue's report is
therefore not a property of one application: with this payload, every iOS
program that reaches those inline paths fails at link.

### 5.2 The constraint, and the mechanism the engine already has

Rule C says the headers, the module and the runtime are one libc++. The
comment in `distribution.cppm` says the runtime can only be the SDK's. Both
cannot hold while `import std` is to be kept, because the SDK's libc++ is a
version the payload carries no module sources for. The first draft of this
record accepted the comment and moved the headers to the SDK, leaving the
std module to a measurement of whether the SDK ships `std.cppm`. That was
wrong in its premise: "no other option" is a statement about the *payload as
published*, not about what the dependency graph can supply, and the engine
already has the mechanism for a graph package to be the C++ runtime beneath
the compiler.

`mcpplibs/openkal-llvm-runtime` is that mechanism in use: libc++, libc++abi
and libunwind as a source package, configured for openkal-musl, declaring
`provides = ["hosted-standard-library", "mcpp:c++-abi=libc++",
"mcpp:compiler-runtime=compiler-rt"]`, `std-module = "…/std.cppm"`,
`std-compat-module` and `std-module-flags`. The engine resolves the five
layers of the target side independently (`targetside_model.cppm:492-563`;
`TargetSide{compiler, compilerRuntime, kernelAbi, cAbi, cxx}`), broadcasts a
graph-origin layer's include directories into every package's private build
(`prepare.cppm:9563-9617`, `note_layer(CxxAbi, …)`), adopts the package's
std module over the toolchain's (`prepare.cppm:10362-10406`), and resolves
the distribution contract to `SelfContained` with `-nostdlib++` whenever the
C++ runtime is the graph's (`distribution.cppm:455-477`). The same code
serves the bare rows, where `mcpplibs/picolibc` is the C library and
`llvm.compiler-rt-builtins` the compiler runtime, both from source, both
compiled with the consuming program's own flags.

What keeps that mechanism from serving the iOS rows is one proxy. Four
sites ask "does the payload's C++ runtime apply" and answer it with the
C-library question `cAbi.prebuilt()`:

| site | what it decides | reads |
|---|---|---|
| `hostflags.cppm:341` | whether the payload's libc++ `-isystem` block is emitted | `!cAbiPrebuilt` |
| `flags.cppm:616` | the same, on the compile-flag builder | `cAbi.prebuilt()` |
| `flags.cppm:660-694` | whether the payload's link-side driver flags are emitted | `cAbi.prebuilt()` |
| `flags.cppm:1166` | `graphCxxRuntime`, the input of the contract's early `SelfContained` branch | `!cAbi.prebuilt()` |

The two questions coincide for openkal (both layers from the graph) and for
a native build (both from the payload). They come apart on the iOS rows,
where the C library is the located SDK (`Origin::Payload`, prebuilt) and the
C++ runtime can be a graph package. `check_layering`
(`targetside_model.cppm:584-600`) already refuses the opposite pairing, a
payload C++ runtime over a graph C library, so `cxx.fromGraph()` is the
question, and it is already computed.

### 5.3 Decision

**The C++ runtime of the iOS rows is a graph package, `llvm.libcxx`, and
the engine asks the C++ layer's own question at the four sites.** The
package is libc++ and libc++abi from `llvmorg-22.1.8` as source, with a
generated `__config_site`, `__assertion_handler`, `std.cppm` and
`std.compat.cppm`, under the namespace and version rule the
`llvm.compiler-rt-builtins` package established (upstream's namespace,
upstream's version, a fourth segment for the packaging). It is not
Apple-specific: libc++ recognises Darwin and glibc by itself, so one
configuration serves a hosted target whose payload cannot supply a static
libc++ for it, which today is the three iOS rows and tomorrow may be others.
libunwind is not carried: on Apple platforms the unwinder is libSystem's,
and on Linux the payload's is linked by the driver.

A framework declares it once, under the rows that need it, and every
application inherits it through the ordinary dependency edge:

```toml
[target.'cfg(os = "ios")'.dependencies]
llvm.libcxx               = "22.1.8.1"
llvm.compiler-rt-builtins = "22.1.8.5"
```

An application without a framework writes the same three lines. The two
packages stay separate for the reason the builtins package itself records:
a C program over the same rows needs the second and not the first, and one
edge per layer is what lets either be replaced. This is the
openkal shape (a program declares `openkal-llvm-runtime`) and the shape the
bare rows have for `compiler-rt-builtins`; the engine injects no dependency
and the row table names none. The alternatives and why they are not taken:

- **A prebuilt payload built on a macOS runner** (`xim:llvm-apple-runtimes`,
  the shape of this record's first revision). It needs a build farm the
  ecosystem does not have, one archive per Apple platform and architecture,
  and a version rule tying the payload to the archives. A source package
  needs none of these and is what the engine already consumes.
- **The SDK's libc++ with module sources of the SDK's version.** It ties
  every object to the machine's Xcode, adds a version-keyed package family
  that has to follow Apple's releases, and puts the C++ runtime on Apple's
  side of the seam the 2026-09-11 record drew ("the compiler is ours; only
  the SDK is Apple's"). It remains the shape of an explicit
  `cxx_runtime = "host-coupled"` request and is not built here.
- **Pinning the payload's libc++ to the SDK's.** A payload cannot know which
  SDK a machine has. The `llvm@20.1.7` workaround is this alternative done by
  hand, and it holds only until the next SDK.

**What the engine does when no package is declared.** The runtime is the
SDK's. A graph that does not import `std` takes the SDK's headers
(`-nostdinc++ -isystem <sdk>/usr/include/c++/v1`; clang's Darwin driver
would otherwise prefer the libc++ installed beside the compiler): one libc++
on every line, which Rule C asks for. A graph that imports `std` keeps the
payload's module and headers over the SDK's dylib, which is what every iOS
build got before this batch; the SDKs ship no module sources (measured
above) and the engine does not consume one, so there is no consistent pair
to switch to. That pairing is reported once as a degradation
(`target/cxx-runtime`) naming the hazard and the two package lines, and the
build proceeds. A refusal was written first and withdrawn on review: it
would have broken a program that built the day before, while the
degradation names the remedy at the first build and costs nothing until an
inline path reaches an export the older dylib lacks.

The engine change, in full:

1. `HostFlagOptions` gains `cxxFromGraph`, read from
   `plan.targetSide.cxx.fromGraph()`. The payload's libc++ `-isystem` block
   is withheld when it is true, and `-nostdinc++` is emitted so that the
   driver's own C++ search (the payload's headers beside the compiler, or the
   SDK's) contributes nothing; the package's headers arrive through the
   layer broadcast that already exists.
2. `flags.cppm:1166` reads `cxx.fromGraph()`. The contract's early branch
   then returns `SelfContained` with `-nostdlib++` for an iOS row that
   declares the package, before the Apple-cross branch is reached. That
   branch stays for the no-package case and its comment is corrected: the
   SDK is the only runtime *the payload* can offer.
3. `flags.cppm:660-694` is unchanged: the link-side driver flags are the C
   library's business, and `-stdlib=libc++` beside `-nostdlib++` is inert.
4. `graph_runtime_compile_flags` (`model.cppm:491-530`) emits
   `-femulated-tls` on Mach-O only when the C library is the graph's. It was
   written for openkal-macos, which has no dynamic loader to bootstrap a
   thread-local; iOS has one. The visibility flags stay for every Mach-O,
   since a hidden copy is what keeps dyld from unifying it with the system
   libc++ that UIKit loads (the #117 forensics).
5. The std-module adoption at `prepare.cppm:10362` accepts
   `mcpp:c++-abi=libc++` as the current spelling of `hosted-standard-library`
   and continues to accept the older one.
6. On an Apple cross target without a graph C++ runtime: when the graph
   does not import `std`, `Toolchain::appleSdkCxxHeaders` is set and the
   compile side emits `-nostdinc++ -isystem <sdk>/usr/include/c++/v1`; when
   it does, the payload's module stays and prepare reports the
   `target/cxx-runtime` degradation naming `llvm.libcxx` and
   `llvm.compiler-rt-builtins`.
7. The builtins archive. Clang's Darwin driver adds
   `libclang_rt.<platform>.a` from its own resource directory and, when the
   file is absent, continues without it (its source says missing runtime
   libraries are tolerated so that a build without compiler-rt can proceed);
   that is the issue's `__isPlatformVersionAtLeast` undefined at link with no
   earlier message. The measurement above settles which half owns the fix:
   the 22.1.8 payload ships no iOS archive, so the compiler runtime of the
   iOS rows is a graph package as it is on the bare rows.
   `llvm.compiler-rt-builtins` gains an Apple source selection
   (`os_version_check.c`, which is where `__isPlatformVersionAtLeast` lives,
   and the generic routines for `aarch64` and `x86_64`; the exclusions the
   openkal package measured for Mach-O), and the framework declares it
   beside `llvm.libcxx`. The engine's part is the layer model's: on an Apple
   cross target whose payload carries no archive for the platform and whose
   graph declares no `mcpp:compiler-runtime`, `TargetSide::compilerRuntime`
   is unsupplied, and prepare reports it once as a degradation naming the
   platform, the missing file and the package that supplies it. A refusal
   was considered and rejected: a program that never reaches an availability
   check links and runs today, and refusing it would trade a diagnosed hazard
   for a regression. The engine never looks in Xcode for the archive.
8. The per-row `toolchain = "llvm@20.1.7"` pin and the builtins lookup
   disappear from every iOS application.

### 5.4 Criteria

The combination this item creates, a prebuilt C library under a graph C++
runtime, is testable on a Linux host with the same package, which is where
the engine's part is measured first:

- Linux x86_64, `llvm@22.1.8`, a program that declares `llvm.libcxx` and
  imports `std`: builds, `ldd` lists no `libc++`, the program runs; the
  target-side report names `c++-abi libc++ (llvm.libcxx@22.1.8.1, graph)`
  and `c-abi glibc (…, payload)`; the compile command carries `-nostdinc++`
  and no payload `-isystem …/c++/v1`; the link command carries `-nostdlib++`.
- Negative direction, same host: the same program without the declaration
  builds against the payload's libc++ exactly as today, byte for byte on
  both command lines. Without this row the change could be read as "every
  Linux build lost the payload's headers".
- On a macOS runner, `aarch64-ios-sim` with `llvm.libcxx` declared: a program
  that imports `std`, uses `std::unordered_map<std::string, int>` and
  `std::atomic<int>::wait`, links and prints under `simctl`; `otool -L` of
  the artefact does not list `/usr/lib/libc++.1.dylib`; the link command
  carries `-nostdlib++` and no `-lc++`.
- `aarch64-macos` with a floor: command lines unchanged from today, byte for
  byte (the `SelfContained` row must not move).
- `aarch64-ios-sim` without the declaration: a program that does not import
  `std` carries `-isystem <sdk>/usr/include/c++/v1` and no payload
  `-isystem`, links `-lc++` and prints no `target/cxx-runtime` line; one that
  does still builds and the degradation names `llvm.libcxx`.
- Builtins: `aarch64-ios-sim` with `llvm.compiler-rt-builtins` declared and
  a program whose source contains `if (__builtin_available(iOS 17, *))`
  links and runs; without the declaration the same program fails at link
  with `__isPlatformVersionAtLeast` undefined and prepare has printed the
  degradation naming the package. Negative direction: `aarch64-macos`
  prints no such degradation, since the payload carries `libclang_rt.osx.a`.
- The CI fixture's `toolchain = "llvm@20.1.7"` line, where one exists, is
  deleted as part of the change, so the test measures the default payload.

### 5.5 What lands where

| piece | repository | size |
|---|---|---|
| `llvm.libcxx` 22.1.8.1: libc++ and libc++abi sources at `llvmorg-22.1.8`, generated configuration and module sources, manifest, examples, CI on Linux and on the iOS simulator | new repository `mcpplibs/libcxx` | one package |
| index entry, GitHub and GitCode release assets | `mcpp-index`, `mcpp-res` | one PR, one release |
| `cxxFromGraph` at the four sites, `-femulated-tls` narrowed, the older and current capability spellings, the SDK-header fallback, the builtins refusal, tests | mcpp | part of the batch PR |
| the Apple source selection (22.1.8.5) | `mcpplibs/compiler-rt-builtins` | one PR, one release, one index entry |
| `docs/20` iOS rows and `docs/22`; the 2026-09-11 record's table gains a superseded note pointing here | mcpp docs | text |

The order between the repositories is the one every package-plus-engine
change has taken: the package is published and indexed first, the engine
PR's fixtures declare it (by `git` until the index carries it, by version
after), and the sandbox measures the released pair.

## 6. Item 5: a known-key list that the parser does not read

### 6.1 What the code does

`toml.cppm:2635-2645` reads `min_api_level`; `2744-2747` defines
`kKnownTargetScalars = {cxx_runtime, linkage, sysroot, toolchain}` and
`kKnownTargetArrays = {runner}`; the sweep at `2748-2760` pushes "has
unsupported key '…' (ignored)" for every scalar not in the list and prints
the same list in the message. Tables are exempt from the sweep, which is why
`abi`, `runtime`, `build`, `xlings` and the rest are unaffected;
`min_api_level` is the only parsed non-table key that is missing. The
warning is `manifest/schema`, and `--strict` turns it into a failure
(`prepare.cppm:2058-2061`). e2e 641 asserts the value-range error and not the
absence of the schema warning, so it did not see the drift.

### 6.2 Decision

The one-line fix now: add `min_api_level` to `kKnownTargetScalars` and to
the message. The structural fix behind it, so that the list cannot drift
again (Rule D): the `[target.<triple>]` scalar parser becomes a table of
`{key, handler}` and the sweep reads the same table, in the shape the
`build.mcpp` directive table already has (`directives.cppm`). The table is
the smaller change once the one-liner is in, and it removes the "Supported
keys:" string as a third copy.

### 6.3 Criteria

- e2e 641 gains an assertion: a manifest with `min_api_level = 24` under
  `mcpp build --strict --target <non-Android row>` exits 0 and its output
  contains no `unsupported key`.
- Unit, with a denominator taken from the code: every `body.find("<key>")`
  in the `[target.<triple>]` parser names a key present in the table, and
  every table key has a parse site. The denominator is read from the source
  tree, not written into the test, so a fifth key added without a table row
  fails the test rather than a build.

## 7. Item 6: the tool store's key holds no source

### 7.1 What the code does

`tool_store::Key` (`tool_store.cppm:77-97`) is `{epoch, indexName,
packageName, version, targetName, hostTriple, compilerIdentity, profile,
features, upstreamKeys}`, the last being `name@version` strings of the
transitive closure; `entry_dir` is
`<cache>/tool/<index>/<pkg>@<version>/<hash>/` (`178-183`). `prepare.cppm:8721-8745`
fills it from `[package] version` unconditionally; a store hit
(`entry_valid`, `8753`) skips the sub-build. `DepCacheIdentity` already
carries `sourceKind` (`4590-4599`) and the object cache uses it to keep
`path` and `git` packages out of the store (`11936-11938`), for the reason
`cache_key.cppm:44-49` states: they are "the only ones that can change
private flags without a version bump". The tool store applies no such rule.
The 2026-08-05 design for #355 specified the upstream axis as recursive
cache keys; what shipped was `name@version`, which for a `path` package is a
constant.

### 7.2 Decision

The source kind enters the decision, mirroring the object cache:

| tool package source | key | store |
|---|---|---|
| index (`version`) | as today | as today: one version is immutable |
| `git` | `version` gains `+git.<commit>`, the commit the dependency resolved to (the identity the lock already computes) | stored: a commit is immutable content |
| `path` | `version` gains `+path.<stamp>`, a hash over every regular file's relative path, size and modification time in sorted order, with `target/`, `.git/`, `.mcpp/` and the compile database excluded | stored: a stamp names one state of the tree, and an edit or its reversal each name another |

The stamp is the answer to two constraints that pull apart. The criterion
needs both directions to move (an edit and its reversal each reach the
consumer), and e2e 187 asserts that a path tool is not rebuilt when nothing
changed. A store that never hits for `path` satisfies the first and fails the
second; a tree hash over file contents satisfies both at the cost of reading
every source on every build. The stat stamp satisfies both at one `stat` per
file, and it is what ninja itself trusts. Stale entries accumulate in the
store as the tree is edited, which `mcpp cache clean` already handles.

`upstreamKeys` takes the same treatment per upstream, so that a `path` or
`git` package two levels below a tool moves the tool's key when it changes,
which is the case the comment on that field already describes.

### 7.3 Criteria

The 2026-09-08 measurement fixes the shape of the test: same path, different
bytes, both directions. With `examples/12`: edit the tool's emitter without
touching its version, build, and the consumer's output changes; revert the
edit, build, and it changes back. A version bump cannot be the probe, since
the tool's path is on the action's command line and the edge would rerun
regardless. For `git`: two commits of one tool repository, the root's `rev`
moved from one to the other and back, produce two store entries and the
consumer's output follows the pin. Negative direction: an index-sourced tool
builds once across two consuming projects (the store path is shared and the
"Building host tool" line appears once).

## 8. Items 7 and A8: the descriptor's three blocks, and where a payload is declared

### 8.1 Item 7: what the code does, and what the consumer actually experiences

`emit_xpkg` fills `xpm.<platform>.deps` from `manifest.xlings.workspaceByPlatform`,
the top-level table (`publisher.cppm:234-240`), and for every
`[target.'<selector>'.xlings.workspace]` prints the `publish/target-axis-tools`
warning explaining that "a selector is not a platform" and that consumers
install what the three blocks name (`215-233`). The issue reports this
correctly and proposes deriving the blocks from the target axis and from
reexported host modules.

Two facts change the consequence and the shape. First, a consumer's own
`mcpp build` loads each dependency's manifest, folds its target-axis tables
for the resolved target (`merge_conditional_config` at `7009-7012` calls
`merge_conditional_xlings`, `265-294`) and provisions the result through the
graph pass (`4541-4560`, `provision_xlings_addresses`). The GTK closure
declared on the rule package therefore reaches a consumer at build time
whether or not the descriptor names it; the descriptor decides what
`xlings install <package>` installs *before* any build, which matters for a
sandbox that installs and then builds offline, and for the auto-install gate
when it is closed. The failure mode is "installed later, or refused with the
package named", not "fails to link". This should be measured once in the
sandbox with auto-install off before the sentence enters `docs/`.

Second, no reexport walk is needed at emit time: every mcpp package on the
index carries its own descriptor, and the consumer's mcpp resolves the rule
package as a package of its own, whose descriptor then names GTK once item 7
is fixed for it. Deriving a library's blocks from its host modules would put
the same payload in two descriptors.

### 8.2 Decision

A selector that names only an operating system is a platform. `cfg(linux)`,
`cfg(os = "linux")`, `cfg(windows)`, `cfg(os = "windows")`, `cfg(macos)`,
`cfg(os = "macos")` and `cfg(unix)` (the first and third blocks) map onto the
descriptor's blocks; a predicate that mentions an architecture, an
environment, a layer or a feature keeps today's warning, whose text gains
the sentence "an OS-only selector is emitted; this one is not". The
`merge_conditional_xlings` rule for one package at two versions across the
axes (`xlings/axis-override`) applies to the emitted block as it does to the
build.

### 8.3 Criteria

- `mcpp emit xpkg` of a manifest with `[target.'cfg(linux)'.xlings.workspace]`
  naming `xim:gtk4`: the `linux` block contains it, the other two do not,
  and no warning is printed for that section.
- The same manifest with `cfg(target_arch = "aarch64")`: no block contains
  the entry and the warning is printed, so the residual case is exercised.
- In the sandbox with auto-install off: install the published package,
  build a consumer offline, and read whether the build refuses naming the
  payload or links; the sentence in `docs/` is written from that reading.

### 8.4 A8: declined again, and why the reason is a rule

`fillXpkgDirs` (`5680-5751`) makes visible to a build program the owner's
own `[xlings.workspace]` and `[feature-xlings]` plus those of every host
module compiled into that build program; an ordinary library edge contributes
nothing. The 2026-09-12 record §2.8 read the issue's A8 as a declaration on
the wrong package and moved one line in HuxerUI. The present issue restates
the ask as "a reexported build-dependency's declarations reaching the
consumer would let an ordinary library declare its own payloads".

The reason to decline is the same and is better stated as a rule: **a
payload is declared by the package whose code consumes it.** A build program
consumes a payload through `xpkg_dir`, so the declaration belongs to the
host module the build program is compiled from. A library consumes a payload
by compiling and linking against it, and declares it on its own target axis,
where the consumer's mcpp already folds and provisions it (§8.1). There is no
third consumer, so there is no third place. What the issue calls a workaround
(the GTK table living in `huxerui-build-rules-gtk`) is the designed form, and
`docs/31` already says so (lines 341-349), so nothing moves.

## 9. Item A9: the universal APK is the library route applied to an app

`build_and_pack_library` takes a list of triples, prepares and builds each,
and stages the legs into one tree (`library_pipeline.cppm:113-`); the program
route refuses a second `--target` with "packing one executable for several
triples would need several executables" (`cmd_publish.cppm:83-86`,
`124-129`). Both are right for what they name. On the Android rows an `app`
target's artifact is `lib<name>.so` (2026-09-12 record, A3), which is the
library route's input.

**Decision.** The route is chosen by the artifact's form, not by the target's
kind: an `app` whose resolved format is a shared object accepts several
triples and is staged as `lib/<abi>/lib<name>.so` per leg, the ABI derived
from the triple's architecture as the 2026-09-12 record already specifies
(`aarch64` to `arm64-v8a`, `x86_64` to `x86_64`). One stage, one dispatch,
one APK. A universal Mach-O is a different mechanism (one fat file produced
by `lipo`) and is not this item.

**Criteria.** `mcpp pack --format apk --target aarch64-linux-android --target
x86_64-linux-android`: one APK whose listing contains both `lib/arm64-v8a/`
and `lib/x86_64/`; with one triple, the APK is byte-identical to today's.
Negative direction: two triples for an `app` on a row whose artifact is an
executable (`x86_64-linux`) remain refused with today's message.

## 10. Order, and what the ecosystem side deletes

The order is by the size of the change and by what each unblocks; the issue's
suggested order is kept where it does not conflict with a dependency between
items.

| step | item | size | what HuxerUI or Lib-Live2D deletes when it lands |
|---|---|---|---|
| 1 | 5 | one line, one assertion; then the table | nothing; the warning stops |
| 2 | 1 + 2 | one `switch` at the resolve hit, one docs table | `git = "/local/path", branch = …` in every application developed beside the framework |
| 3 | 3a | a reorder in `pack::run`, one manifest field | nothing yet; `dist-apple` gains a resource destination in `mcpp:plugins` |
| 4 | 6 | source kind in the tool store | the version bump on every tool change |
| 5 | 4, recipe | `xim:llvm-apple-runtimes@<version>` in `xim-pkgindex`, built on a macOS runner | nothing yet |
| 6 | 4, engine | row column, install through the pin's channel, archives and builtins by path, `appleFloor`, the Apple-cross `HostCoupled` branch removed, one refusal | `toolchain = "llvm@20.1.7"` in every iOS application; the `libclang_rt.iossim.a` lookup and its Xcode fallthrough in the build program |
| 7 | 3b | the Mach-O reader; bundling after a measurement | nothing yet; the `.app` becomes runnable with its dylibs |
| 8 | 7 | OS-only selectors map to blocks | nothing; the descriptor becomes complete for the rule package |
| 9 | A9 | route by artifact form | two builds and a hand-merged APK |
| - | A8 | declined | nothing; `docs/31` already carries the sentence |

Each step keeps its own criterion, so that none of them is folded into a
neighbour and lost when the neighbour ships (the 2026-09-12 record's rule
7). The engine steps land in one mcpp PR, which is the repository's rule for
a batch; step 5 precedes step 6 across repositories because the engine PR
declares a package that must already be indexed.

## 11. Self-review

- **Statements about the present.** Every "the code does" clause names a
  line at `b63dc4e5`. The first draft deferred two measurements (whether the
  iOS SDK ships module sources; whether the payload ships the iOS builtins);
  the runtime package of §5.3 makes both irrelevant to the default path, and
  the first survives only as a question for route S, which is not built
  here. One consequence is deferred to the sandbox: what a consumer
  experiences when the descriptor under-states a payload (§8.1).
- **Where the issue was corrected.** Item 1's rule is queue order, not the
  root (§2.1). Item 3a's refusal already reaches the dispatch, with an empty
  tree (§3.1). Item 7's consequence is install-time, not link-time, and the
  reexport walk is unnecessary (§8.1). A8 was decided in the 2026-09-12
  record and the reason is restated as a rule (§8.4). Item 4's premise is
  confirmed and the 2026-09-11 record's contrary sentence is identified
  (§5.1).
- **Where the issue's proposal was not taken.** `[patch]` (§2.2); using the
  SDK's libc++ headers and pinning the payload to them (§5.2, routes S and
  N, recorded and not built); deriving descriptor blocks from reexported
  host modules (§8.1).
- **Negative directions.** Every criterion section has one, since each of
  these checks can pass while measuring nothing: a warning emitted
  unconditionally (§2.4), a tar that silently drops the closure (§3.3), a
  reader that reads the wrong slice (§4.3), a refusal that never runs (§5.4),
  a test whose denominator is written by hand (§6.3), a probe that reruns for
  the wrong reason (§7.3), a residual warning that disappeared (§8.3), a
  route that widened to executables (§9).

## 12. Task list, dependencies and repositories

The batch is one mcpp PR (the repository's rule for a batch of engine
changes), one new package repository, one index PR, one plugins PR, and
a release with both mirrors. Tasks are listed with what they depend on so
that none is left half done when its neighbour ships.

| id | task | repository | depends on | criterion (§) |
|---|---|---|---|---|
| T1 | `min_api_level` in the known list and the message; e2e 641 asserts no schema warning under `--strict`; a unit test whose denominator is the parser's `body.find` sites | mcpp | - | §6.3 |
| T2 | OS-only selectors map to the descriptor's platform blocks; residual warning text; unit or e2e for both directions | mcpp | - | §8.3 |
| T3 | `ResolvedRecord.sourceRef`/`fromRoot`; the six-row switch at the resolve hit; `dependency/source-override`; the constraint check against a checkout's version; `docs/05` en and zh table; e2e fixture with local git repositories, six cases | mcpp | - | §2.4 |
| T4 | `pack::run` reorder: stage program and declared files before the closure; stage manifest `closure`; pipeline warning text; tar/dir keep refusing; e2e byte-identical ELF tree | mcpp | - | §3.3 |
| T5 | `needed_names` for Mach-O (thin and fat), `@rpath` resolution, Mach-O row of `is_system_lib`; unit tests with checked-in Mach-O fixtures; the macOS e2e | mcpp | T4 | §4.3 |
| T6 | tool store: `git` keyed by commit, `path` never a hit; `upstreamKeys` per source kind; e2e in both directions with `examples/12` | mcpp | - | §7.3 |
| T7 | `llvm.libcxx` 22.1.8.1: repository, sources at `llvmorg-22.1.8`, generated configuration and module sources, manifest, examples, CI (Linux with `llvm@22.1.8`; macOS runner for `aarch64-ios-sim`) | `mcpplibs/libcxx` (new) | - | §5.4 |
| T7b | `llvm.compiler-rt-builtins` 22.1.8.5: the Apple source selection under `cfg(os = "ios")` and `cfg(os = "macos")`; CI on a macOS runner | `mcpplibs/compiler-rt-builtins` | - | §5.4 |
| T8 | engine: `cxxFromGraph` at the four sites; `-femulated-tls` narrowed; both capability spellings; SDK-header fallback and the std-module diagnostic; the unsupplied compiler-runtime degradation; Linux e2e with T7 by `git`; iOS CI fixture declares T7 and T7b | mcpp | T7, T7b | §5.4 |
| T9 | route by artifact form: an `app` whose artifact is a shared object takes the library route's several triples; `lib/<abi>/` staging; e2e on the Android rows | mcpp | - | §9 |
| T10 | docs: `docs/05` (T3), `docs/20` and `docs/22` iOS rows (T8), `docs/30` stage manifest field (T4), zh mirrors; the 2026-09-11 record's superseded note | mcpp | T3, T4, T8 | structure and parity checks |
| T11 | `mcpp-index`: `llvm.libcxx` entry and the `llvm.compiler-rt-builtins` 22.1.8.5 entry (GitHub and GitCode assets); `mcpp-res` releases | `mcpp-index`, `mcpp-res` | T7, T7b | index `latest` names them; sandbox install |
| T12 | `mcpp:plugins`: `dist-apple` places the staged tree's deployed files at the bundle's resource destination | `mcpp-plugins` | T4 released | the `.app` carries the deployed file |
| T13 | release mcpp; bump the workspace pin; GitCode assets by `gtc`; index bump PR | mcpp, `mcpp-index` | T1-T10 merged, CI green | `origin/main` HEAD run green; sandbox `mcpp --version` |
| T14 | sandbox verification with `xlings subos … --sandbox --cmd`, CN mirror configured for both tools: T3 warning, T6 rebuild, T8 Linux program, T2 descriptor, T1 silence | sandbox | T11, T13 | one ok/FAILED line per claim |
| T15 | this record's status and §13 "what landed and where it was measured" | mcpp docs | T14 | - |

Parallel groups: {T1, T2}, {T3}, {T4 then T5}, {T6, T9}, {T7 then T8} can
proceed at once; T10 follows its inputs; T11 follows T7; T12 follows the
release; T13 to T15 are sequential.

**Progress (2026-09-13, evening).** T1 to T8 and T10 are on the batch branch
(mcpp-community/mcpp#631). T7 is published (`mcpplibs/libcxx`, tag
22.1.8.1; GitCode mirror byte-identical). T7b is merged and tagged
(mcpplibs/compiler-rt-builtins#1, 22.1.8.5). T11 is merged and published
(mcpplibs/mcpp-index#408; a program resolving `llvm.libcxx = "22.1.8.1"`
from the index built and ran on Linux). T12 is prepared on a plugins
branch and waits for the release pin. T9, T13, T14 and T15 follow.

## 13. What landed, and where each claim was measured

| item | landed as | measured |
|---|---|---|
| 1, 2 | `prepare.cppm`: `ResolvedRecord.sourceRef`/`fromRoot`, the six-row decision at the resolve hit, `dependency/source-override`; `docs/05` en and zh | e2e 661, six cases, on Linux, macOS and Windows shards of #631 |
| 3a | `pack.cppm`: `stage_declared` before the closure, `finish_without_closure`, `closure_unavailable_outcome`; `stage_tree.cppm`: `ClosureStatus` in the manifest; `pipeline.cppm`: the tree is handed over with `closure = not-walked` | e2e 662 (Linux), e2e 666 (macOS: a Mach-O program reaches a dispatched format with the deployed file and `closure = not-walked`; `--format dir` still refuses), unit tests for the outcome function and the manifest |
| 3b | `binfmt.cppm`: `macho_needed` (thin and fat, both byte orders), `resolve_macho_names`, the Mach-O row of `is_system_lib`; `needed_names` dispatches to it. The closure step still reports `not-walked` for Mach-O; bundling waits for the `LC_RPATH` measurement | `test_pack_binfmt` over generated fixtures |
| 4 | `hostflags.cppm`/`flags.cppm`: `cxxFromGraph` and `appleSdkCxxHeaders`; `model.cppm`: `-femulated-tls` only when the C library is the graph's; `prepare.cppm`: both capability spellings, the `-isysroot` on the package std module's command, the `target/cxx-runtime` and `target/compiler-runtime` degradations, `payloadCompilerRuntimeAbsent`; `docs/20` en and zh | e2e 663 on Linux (glibc under `llvm.libcxx`, both directions); `ci-macos-ios` on #631: `aarch64-ios` and `aarch64-ios-sim` build a program that imports `std`, hashes strings, notifies an atomic and takes an availability check, over the two packages, with the report naming `c++-abi libc++ (libcxx@22.1.8.1, graph)` and `compiler-runtime compiler-rt (compiler-rt-builtins@22.1.8.5, graph)`; the first run without `-isysroot` on the std module's command stopped on `mbstate_t`, which is the measurement behind that line |
| 5 | `toml.cppm`: `min_api_level` in the known list and the message; `test_target_scalar_keys` with the parser's own `body.find` sites as the denominator | e2e 641 case 9 under `--strict` |
| 6 | `tool_store.cppm`: `tree_stamp`; `prepare.cppm`: `DepCacheIdentity.sourceRef`, `source_keyed_version` for the tool and its upstreams; `docs/30`, the examples/12 README and the CI probe restated | e2e 665 (both directions, a store hit when unchanged, `git` by commit), e2e 187 unchanged, `test_tool_store`; the examples job of #631, whose first run measured that the old probe edits the tree it later reads and had to build its probe compiler from a copy |
| 7 | `prepare_inputs.cppm`: `cfgpred::os_only_platforms`; `publisher.cppm`: OS-only selectors fill the platform blocks, the warning says so | `test_cfg_os_only_platform`, `test_xpkg_emit` |
| A9 | `route.cppm`: `accepts_several_targets`; `pipeline.cppm`: `build_extra_android_legs`; `pack.cppm`: `lib/<abi>/` per leg; `triple.cppm`: `android_abi` | e2e 664 on the Android rows (two ABIs in one tree, one triple unchanged, executables refused) |
| A8 | declined; `docs/31` already states the rule | - |
| T7, T7b, T11 | `mcpplibs/libcxx` 22.1.8.1; `mcpplibs/compiler-rt-builtins` 22.1.8.5; index entries | GitHub and GitCode archives byte-identical; a Linux program resolving `llvm.libcxx = "22.1.8.1"` from the published index built and printed `1-2-3`; the builtins package's macOS CI reads six symbols out of the simulator archive |

Three things the batch measured that the design did not foresee:

- **The package std module lost the SDK.** The block that adopts a package's
  std module rebuilds `stdModuleTargetFlags` and replaced the `-isysroot`
  the toolchain resolution had put there; the precompile stopped on
  `mbstate_t`. The SDK is now appended in that block (§5.3, the first
  `ci-macos-ios` run).
- **An exclusion in a manifest's source list is global.** The builtins
  package's first Apple revision re-listed five routines the package-wide
  list excluded, and the archive did not carry them; the exclusions moved
  into the five M-profile blocks (compiler-rt-builtins#2).
- **A fixture that stands in for the C++ layer answers for its headers.**
  e2e 304's stub `mcpp:c++-abi` provider included `<cstdio>` and lost the
  payload's libc++ under the new rule, on the one host whose toolchain is
  clang; it now includes the C header. e2e 661 was corrected for the
  fixture-path hygiene rule, and 666 for `import std` under clang.

The design's two refusals were withdrawn on review before landing: a graph
that imports `std` on an Apple cross target without a package builds as it
did yesterday and is reported once (§5.3), and a payload without a builtins
archive is reported once rather than refused (§5.3, item 7).
