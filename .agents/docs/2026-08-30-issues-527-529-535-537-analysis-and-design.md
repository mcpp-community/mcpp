# Four issues, measured: #527 (workspace half), #529, #535, #537

> Status: analysis and design. No code changed.
> Baseline: `origin/main` @ `f4cb9a1`. The working tree at the time of writing
> was 33 commits behind and predates the `modules/` split, so every line anchor
> below was read from a detached worktree at `origin/main`, not from the
> checkout.
> Measured with: mcpp `2026.8.30.1`, gcc 16.1.0, ninja 1.12.1, Linux x86_64,
> 32 threads.
> Reproductions: six, all recorded below, all reduced to inputs that need
> nothing from the index.
>
> **Scope.** Host-native toolchain and sysroot support — #527 RFC 1, and the
> "make `toolchain = "system"` work" reading of #527 Bug 1 — is out of scope by
> instruction and by the maintainer's A1/A3 on the issue. What remains of those
> two items in this document is a diagnostic obligation, not a feature. #537 is
> likewise treated as a truthfulness defect in an existing guard, never as a
> proposal to support host linking.

---

## 0. Verdicts

| # | filed as | verdict | what it actually is |
|---|---|---|---|
| #529 | performance regression in `mcpp test` | **real defect, root-caused** | `prepare_build` rewrites `resolution.json` from a fresh object and drops `runtime.loader_tags` and `runtime.symbol_provision`, so the memoisation both post-link ELF passes were built around is destroyed before it can ever be read across invocations |
| #535 | host tool cannot start | **real defect** | the tool store publishes exactly one file, so a host tool's link unit can carry a `DT_NEEDED` that nothing in the store satisfies; the scratch tree that held the provider is then deleted |
| #537 | link-time and run-time providers diverge silently | **real defect** | the closure guard is satisfiability-based; when a SONAME resolves on both sides it is structurally unable to fire |
| #527 Bug 1 | `posix_spawnp('')` under `toolchain = "system"` | **real defect** | the resolved compiler path exists in `tc->binaryPath` and is not handed to the `build.mcpp` compile; the same manifest without a `build.mcpp` builds fine (measured), so this is an unfilled variable, not the host-support boundary |
| #527 Bug 2 | workspace `[build]` not inherited | **real defect, reproduced** | the two inheritance sites merge four keys and `buildConfig` is not among them |
| #527 RFC 2 | sync dialect flags into the `import std` BMI | **mechanism complete, silence is the defect** | `dialect_cxxflags` already reaches the std BMI prebuild; nothing tells the user that is where the flag belongs |
| #527 RFC 3 | `[workspace.*]` inheritance to prevent cross-package BMI drift | **premise incorrect; a larger defect underneath** | drift is impossible — the root's standard is imposed graph-wide — but a dependency's declared `standard` is therefore accepted and discarded with no diagnostic |
| #527 RFC 4 | aggregate `compile_commands.json` | **real gap, small** | per-member CDBs only; no root aggregate |
| #527 RFC 1 | host-native toolchain and sysroot | **out of scope** | answered A3; not revisited here |
| — | not filed | **real defect, found while reviewing D3** | the fast path's staleness sweep is rooted at the project directory, so a **new source file appearing in a `path` dependency** is invisible: `mcpp build` reports `Finished dev in 0.00s` and the module is never compiled (§2.5, D3a) |

Nothing in this set is a usage error. The closest is #527 RFC 2, where the
correct key exists and the user did not find it — and the reason they did not
find it is that mcpp knows the answer and does not say it, which makes it a
diagnostic defect rather than a usage error.

---

## 1. Two families, and the policy that decides refuse versus warn

### 1.1 Two families

Eight of the nine items reduce to two shapes. Naming them is what keeps the
individual fixes from being eight unrelated patches.

**Family A — a record exists and the decision does not read it.**
`resolution.json` holds verdicts that the next invocation destroys (#529). A
dependency's `standard` is parsed into `Package::standard` and no consumer
reads it (RFC 3). `dialect_cxxflags` is the field that answers the question and
no diagnostic names it (RFC 2). The workspace root's `buildConfig` is loaded
and never merged (Bug 2). The refusal text for a system toolchain is compiled
into the binary and unreachable on the native path (Bug 1).

**Family B — the run-time search path mixes what was declared with what this
machine happens to have, and the artifact never says which one won.**
`runtime_search_closure` (`src/build/plan.cppm:728`) already tags every
directory with a provenance — `Payload`, `Package`, `Artifact`, `SubosFarm`,
`HostDefault` (`modules/platform/src/runtime_search.cppm:45`). The closure
resolver returns the file that satisfied each `DT_NEEDED`. The two facts are
never joined, so "satisfied by a declared dependency" and "satisfied by
whatever is installed on this developer's machine" are the same green build
(#535, #537). #414 is the same shape, already fixed for one directory: the farm
outranked `$ORIGIN`, and the artifact linked one `libX11` and loaded another.

### 1.2 The host-dependence policy, stated once

Family B is a policy question as much as a mechanism, and every diagnostic
proposed below has to answer it the same way or the tool contradicts itself.
The rule, in the maintainer's terms:

> **mcpp itself, and everything the mcpp ecosystem provides, depends on no
> host. Where something must be depended on, it arrives through the xlings
> system — mcpp-index or the xim package index.**
>
> **A user's own project may depend on the host in the LIBRARIES it links —
> its own `.so`, or one the machine provides. That choice is theirs to
> guarantee. It is not recommended, it is warned about, and it is not refused
> as long as the result builds and runs.**
>
> **The TOOLCHAIN is not that axis. mcpp builds only with toolchains it
> manages: `[toolchain] … = "system"` is refused, with `msvc@system` the single
> exception.**

**The rule is per axis, and that is the correction this document needed.** An
earlier draft applied one boundary — "does it build and run" — to everything,
and concluded that `toolchain = "system"` should be warned about because it
does build and run (measured, §7). That conclusion was wrong, and the reason is
not that the measurement was wrong:

> Everything mcpp promises — that `import std` is available, that the runtime
> closure is computable, that two machines and CI produce the same build — is a
> statement about **a compiler mcpp resolved and can identify**. A compiler
> picked off `PATH` makes every one of those unverifiable. The libraries a
> program links carry no such promise: they are the program's own dependencies,
> and the developer who chose them owns the artifact.

So the compiler is part of mcpp's contract and is not the project's to take
from the host, while the libraries are exactly the project's to choose. Applying
that to every item here:

| item | axis | severity |
|---|---|---|
| D15 `toolchain = "system"` | toolchain — mcpp's contract | **error**, `msvc@system` excepted |
| unsatisfiable closure (existing) | library, and provably cannot run | error — unchanged |
| D14 dialect flag not in the BMI | toolchain dialect, provably cannot build | error |
| D6 host tool with an unsatisfiable closure | library, cannot run | error, where the tool is built |
| D8 host or farm-supplied provider | library, runs | warning |
| D13 dependency declares a higher standard | neither; a graph-shape statement that still builds | warning |

Within the library axis the old boundary still holds and is still the right
one: refused when the artifact provably cannot start, warned when it runs.

**A warning about a host LIBRARY names the way back to the ecosystem.** Not "do
not do this", which the developer has already decided; the actionable sentence
is where the supported version of the same thing lives:

```
note: mcpp builds against no host library by default, so this artifact is not
      reproducible on another machine and `mcpp pack` cannot bundle it.
      The supported route is to declare the provider as a dependency from
      mcpp-index. If mcpp-index does not carry it yet, contributing the
      package is the path — see docs/… — and the dependency then resolves
      the same way on every machine and in CI.
```

One vocabulary, used by D8 and the existing `allow_host_libs` note, so a
developer meets the same sentence wherever they touch a host library. Writing
it twice in two wordings is how the two drift. The toolchain refusal is
deliberately NOT that sentence: it is not advice about a trade-off, it is a
statement that the configuration is unsupported.

---

## 2. #529 — `mcpp test` re-derives an unchanged answer on every invocation

### 2.1 What was measured

A two-member workspace, no index dependencies: `big` provides 101 module
interfaces that each `import std;`, `app` takes `big` as a `path` dependency
and has nine tests. All artifacts built and unchanged.

```
warm `mcpp build -p app`   0.695 s
warm `mcpp test  -p app`   1.94 s   (three runs: 1.932, 1.983, 1.946)
```

`MCPP_VERBOSE=1` decomposes the test invocation (the instrumentation added with
the earlier rule-E work, `src/build/ninja_backend.cppm:2643`):

```
build/stage: emit-ninja: 1ms
build/stage: compile-commands: 6ms
build/stage: ninja: 3ms
build/stage: loader-tags: 449ms          ← drive 1
build/stage: symbol-provision: 920ms     ← drive 1
build/stage: emit-ninja: 1ms
build/stage: compile-commands: 6ms
build/stage: ninja: 3ms                  ← drive 2: neither pass appears at all
```

**1.36 s of a 1.94 s invocation — 70 % — is two passes re-reading ten ELF
images totalling 115 MB to reach the answer they reached last time.** Ninja
compiles and links nothing; artifact mtimes are byte-identical before and
after.

The cost scales with link units, measured on the same tree:

| test binaries | loader-tags | symbol-provision |
|---|---|---|
| 3 | 151 ms | 307 ms |
| 9 | 449 ms | 912 ms |

Per link unit it is ~50 ms and ~100 ms for an 11.5 MB image, and the work is a
full read of the image, so it scales with image size — that is, with the
module-interface surface statically linked into it. This is exactly the scaling
law the issue reports ("scales with the module-interface surface of the whole
dependency closure, including workspace `path` dependencies"), and it explains
every one of its side observations: no child processes (the passes are
in-process), nothing written but a handful of `.json` files (the records), and
a 2× spread between runs on the larger member (page-cache sensitivity of
reading hundreds of megabytes).

**And the real developer loop is worse than the steady state.** The figures
above are `mcpp test` repeated. An edit-test loop alternates `mcpp build` and
`mcpp test`, and the two commands write **different plans into the same output
directory** — measured, `mcpp test`'s plan contains only the nine test
binaries and does not contain `bin/app` at all. Alternating them:

```
                 runtime-validate   loader-tags   symbol-provision   wall
build -p app           146 ms           51 ms           101 ms       0.70 s
test  -p app          1192 ms          462 ms           895 ms       3.15 s
build -p app           136 ms           49 ms           101 ms       0.70 s
test  -p app          1172 ms          458 ms           925 ms       3.12 s
test  -p app          (absent)         440 ms           904 ms       1.95 s
```

Two facts, and the second one is the important one for the fix:

- `loader-tags` and `symbol-provision` cost the same in **every** row,
  including the repeat, because their record is wiped unconditionally (§2.2).
- `runtime-validate` costs 1.19 s only after a `build`, and **disappears on the
  repeat**. Its record lives in a sidecar that survives `prepare_build`, so it
  memoises correctly — until the other command prunes it (§2.5, D2).

So the loop a user actually runs pays **2.55 s of the 3.15 s (81 %)** on
re-deriving answers that are already on disk.

**Honest limit of this reproduction.** It accounts for 2.55 s, not for the
reporter's 15.6 s. The mechanism is proven and the scaling law matches; the
absolute figure on their tree is not reproduced here, and the fix below should
be validated against their workspace rather than assumed to close the whole
gap.

### 2.2 Root cause

Both passes were written with a read-back, and the comment on it is precise
(`src/build/runtime_validation.cppm:720-731`):

> Re-parsing every image on every drive is what made the loader-tag check cost
> 158.7 s of a 190 s hot run, so an artifact whose stat did not move is skipped
> here too. But skipping it must not DROP its verdict [...] the record would
> shrink to "whatever moved last".

The read-back reads `<outputDir>/resolution.json`. And
`prepare_build` writes that file from a **fresh** `nlohmann::json` object
(`src/build/prepare.cppm:9283`, written at `:9420-9433`) on every invocation.
The fresh object carries `binding`, `search`, `link_intent`, `validation` and
friends; it carries neither `runtime.loader_tags` nor
`runtime.symbol_provision`. Every invocation therefore begins by deleting the
memo that the invocation's own backend is about to look for.

### 2.3 The control that proves it

Two observations, both direct:

```
$ python3 -c "keys of runtime{} in resolution.json"     # after `mcpp test`
['binding','cxx_runtime_by_role','link_intent','loader_tags','search',
 'symbol_provision','validation']

$ mcpp build -p app --configure-only                     # prepare_build only
$ python3 -c "same"
['binding','cxx_runtime_by_role','link_intent','search','validation']
       loader_tags: absent        symbol_provision: absent
```

And the positive control is in the verbose trace above: within one invocation,
**drive 2 pays nothing** — neither pass prints a line, so each cost under 1 ms
— because drive 1 wrote the record and nothing wiped it in between. The
memoisation works. It is only ever destroyed across the process boundary.

The third control is `validate_changed_artifacts`, the one post-link pass that
does *not* record into `resolution.json`: it keeps its own sidecar,
`.mcpp-runtime-verdicts.json` (`src/build/runtime_validation.cppm:149`), keyed
by `contract_hash`. That file survives `prepare_build`, and on a **repeated**
`mcpp test` the pass costs under 1 ms while its two neighbours cost 1.36 s.
The difference between the fast pass and the slow ones is the file they write
to. The same control also shows the limit of the sidecar as it stands: after an
intervening `mcpp build` it costs 1.19 s, because the two commands prune each
other's entries (§2.5, D2).

### 2.4 Two amplifiers

Neither is the root cause, and both make it worse in exactly the workspace
shape the issue reports.

**`mcpp test` has no fast path.** `cmd_build` short-circuits a fully cached
build through `try_fast_build`, which skips `prepare_build` entirely
(`src/cli/cmd_build.cppm:134-144`). `run_tests` calls `prepare_build`
unconditionally (`src/build/execute.cppm:1349`). This alone is most of the
0.695 s vs 1.94 s gap that is not the two ELF passes.

**`-p` disables the fast path that does exist.** The guard requires
`ov.package_filter.empty()`. Inside a workspace, every per-member command is
spelled `-p <member>`, so no workspace member has ever taken the fast path,
for `build` or for `test`. A `--workspace` sweep pays the full
`prepare_build` plus the full ELF re-derivation once per member.

### 2.5 Design

**D1 — the post-link verdict records move out of `resolution.json` into the
sidecar that already survives.** `loader_tags` and `symbol_provision` join
`.mcpp-runtime-verdicts.json`, under the same `contract_hash` invalidation the
runtime verdicts already use. `resolution.json` goes back to being what its
writer treats it as: a record regenerated per configure, published for `mcpp
why runtime`, CI and `doctor`.

**This is not a new pattern; it is the pattern the file already uses, applied
to the two passes that skipped it.** `sync_resolution_verdict`
(`src/build/runtime_validation.cppm:328`) reads the sidecar and projects the
runtime verdicts into `resolution.json`'s `runtime` block after the link. So
the architecture is already "sidecar is authoritative, `resolution.json` is the
published copy, synced post-link" — `loader_tags` and `symbol_provision` are
simply the two records that were written directly into the published copy
instead, which is why they are the two that get erased.

Finding this also rules out the alternative that looked simpler: teaching
`prepare_build` to preserve the two keys when it rewrites. That would make
`resolution.json` authoritative for two records and a published copy for a
third, in the same `runtime` object. Fewer lines, worse structure, and the next
post-link record would have to guess which convention to follow.

**D1a — the invalidation key must cover what the verdict actually depends on,
and today it does not.** This is the correctness half of D1, and skipping it
would trade a slow correct answer for a fast stale one. A memoised
`loader_tags` / `symbol_provision` verdict is a function of more than the
artifact's stat:

- **The SubOS farm.** `<subos>/lib` is a symlink view rewritten by every
  `xlings install`, and it is in the artifact's runtime search path. Installing
  a package can change which file satisfies a `DT_NEEDED` without touching the
  artifact. Today the verdict is re-derived every run and therefore correct by
  accident. `try_fast_build` already treats `<subosDir>/.xlings.json`'s mtime as
  the farm's version stamp (`src/build/execute.cppm:893-895`); the record must
  use the same stamp.
- **The policy inputs that are not in the fingerprint.** `MCPP_ALLOW_HOST_LIBS`
  is read from the environment at check time (`host_libs_allowed`,
  `src/build/runtime_validation.cppm:194-198`) and does not enter the output
  directory's fingerprint, so it can flip the verdict with every input file
  unchanged.

Both fold into the sidecar's existing `contract_hash` slot as additional key
material. The criterion for this is in §2.6, step 4 — and it is a criterion
that only exists because the memoisation is being made durable; nobody needed
it while the answer was recomputed every time.

**D2 — an artifact leaves the record when it leaves the disk, not when it
leaves the current command's plan.** D1 alone is not sufficient, and the
measurement in §2.1 is why. `mcpp build` and `mcpp test` share one output
directory and have **different link-unit sets** — verified against the emitted
graph: the test plan's `cxx_link` edges are the nine test binaries and
`bin/app` does not appear in it at all. `validate_changed_artifacts` prunes
every entry whose key is absent from the current plan
(`src/build/runtime_validation.cppm:429-438`), so each command deletes the
other's verdicts, and the pass that memoises correctly for a repeated command
still costs 1.19 s in the alternating loop that a developer actually runs.

Moving `loader_tags` and `symbol_provision` into the same file without changing
this would inherit the same behaviour. The pruning is there to keep the file
from growing without bound, which is a real concern; the correct predicate for
it is **the artifact no longer exists on disk**, not "the current plan does not
mention it". Under that predicate the record is a property of the output
directory — which is what it physically is — rather than of whichever command
last ran against it.

This is the same collision that `is_plain_build_graph` (#407) had to solve for
`build.ninja`: `mcpp test` and `mcpp build --configure-only` write their plan
into the same file as a plain build, and the file needed a discriminator. The
verdict record has the identical collision and does not have one.

**D3 — extend the fast path to workspace members, but not before the hole
below is closed.** The guard's purpose is to refuse the fast path when an
override would be silently ignored; `package_filter` is not such an override —
it selects which project directory is being built, which the fast path can
honour by resolving the member directory first and keying the freshness check
on that directory's `build.ninja`.

**The precondition is a defect found while reviewing this proposal, and it is
not in any of the four issues.** `sources_newer_than` sweeps only
`projectRoot/src/**/*`, `projectRoot/build.mcpp`, the resource scripts, and
`glob_inputs_stale(projectRoot)` — all rooted at the project being built
(`src/build/execute.cppm:659-712`). A `path` dependency's sources are outside
all four. Content edits are still caught, but not by the sweep: ninja rebuilds
the dependency object and relinks, and `artifact_snapshot_unchanged` then
abandons the fast path after the fact (`:917-919`). What nothing catches is a
**new file appearing** in a `path` dependency — the #359 shape ("a GLOB input
changes without any existing file's mtime changing"), whose fix was bounded to
the project root. Measured on a single-package project with one `path`
dependency:

```
$ printf 'export module dep.third;\nexport int three(){return 3;}\n' > ../dep/src/third.cppm
$ mcpp build
    Finished dev in 0.00s
$ find . ../dep -name 'dep.third*'
(nothing — the module was never compiled, and the build reported success)
```

Today this is narrow: it needs a single-package project with a `path`
dependency. **D3 would make it the normal case**, because members of a
workspace depend on each other by `path` and every workspace command is spelled
`-p`. So D3 is conditioned on the sweep first covering every `path` dependency's
source root and manifest — and that is worth doing on its own merits, before
and independently of D3.

**D3a — close the sweep hole regardless of D3.** `sources_newer_than` takes the
set of directories the plan actually reads from, not one project root. The
resolved package list is available at plan time and is already recorded; the
fast path needs the same list, which means recording the dependency source
roots in `.build_cache` alongside the output directory it already stores.

**D4 — a fast path for `mcpp test`.** Lower priority, and it should be built on
top of D1–D3 rather than instead of them: with the two ELF passes memoised and
`prepare_build` skippable, the remaining per-invocation cost is small enough
that the value of a separate test fast path should be re-measured before it is
designed.

### 2.6 Criterion

The assertion must be on **elapsed time attributable to the two stages, with a
denominator**, and it must be taken across two separate process invocations,
because a single invocation already passes today:

1. Build a fixture with a known number of link units *N*, all warm.
2. Run `mcpp test` twice. On the second run, assert that
   `build/stage: loader-tags` and `build/stage: symbol-provision` are **absent
   from the verbose output** (each below the 1 ms print threshold), and assert
   that `N` entries are present in the record — the denominator, so that "the
   pass was skipped" cannot be satisfied by "the pass had nothing to look at".
3. Touch exactly one source belonging to exactly one link unit. Assert the
   record still has `N` entries and that the changed one has a fresh verdict.
   This is the step that catches the cheap wrong fix, in which skipping
   unchanged artifacts shrinks the record to what was relinked.
4. **Alternate `mcpp build` and `mcpp test`**, and assert on the second `test`
   that all three stages are absent and the record holds *N + M* entries — the
   union of both plans, not either one. Without this step D1 passes and D2 does
   not exist, and the loop a developer runs keeps paying (§2.1).
5. **Staleness, which the memoisation newly makes possible.** With everything
   warm, change the SubOS farm (an `xlings install` of a package providing a
   SONAME already in the closure) without touching any project file, and assert
   the verdict is recomputed. Then set `MCPP_ALLOW_HOST_LIBS=1` on an otherwise
   identical invocation and assert the same. Both must fail against a
   stat-only key, which is what makes them a criterion for D1a rather than a
   restatement of steps 2–4.

Step 3 is the same trap the earlier rule-E work recorded: a purely no-op
regression test cannot distinguish a correct memoisation from a record that has
quietly become "what moved last". Step 4 is the trap this review found: a
memoisation can be correct for a repeated command and worthless for the pair of
commands anyone actually alternates.

**Separate criterion for D3a**, because it is a different defect: add a new
source file to a `path` dependency, changing nothing else, and assert the file
is compiled and appears in the dependency's archive. Assert also that the
*count* of objects in that archive went up by one — "it rebuilt" is satisfied
by a full rebuild that happens to be triggered for another reason.

---

## 3. #535 — a host tool cannot carry a runtime closure

### 3.1 What the store publishes

`src/build/prepare.cppm:6699-6731`. After the sub-build succeeds, the
provisioning pass copies **one file**: the tool executable, from the sub-build's
output directory into `<store>/bin/`. It then removes the sub-build scratch
tree. If the tool's link unit has a `DT_NEEDED` on a `kind = "shared"`
dependency's output, the file that satisfied it lived in the scratch tree and
no longer exists anywhere the tool will look.

This is not a missing copy statement. **A store entry whose published form is a
single file cannot represent a program that is more than one file**, and the
entry validator (`tool_store::entry_valid`) accordingly reports such an entry
as valid. The issue's report and this reading agree exactly: `$ORIGIN` is in the
tool's `RPATH` and the directory it names is empty.

### 3.2 The second half, which is the more dangerous one

The tool's `RPATH` ends with the SubOS library view. On a machine where any
package has installed a library with the same SONAME, the missing dependency is
satisfied — by a different build of a different version — and the tool starts.
The issue measured precisely this: `libexpat.so.1` resolved to `xim:expat`
2.6.2 rather than to the declared `compat.expat` 2.7.1, and the build was green
on the developer's machine and red on a clean runner.

That is Family B, and it is the same sentence as #537: the artifact was linked
against one library and loaded another, and nothing said so.

### 3.3 Design

**D5 — a host tool's store entry is a directory, not a file.** Publish the
executable together with the runtime files it needs. Both halves already exist
and should be reused rather than re-derived: `mcpp.build.stage` implements the
publish discipline (content compare, write out of place, rename), and
`plan.runtimeDeployFiles` / `LinkIntent::deployFiles` is already the answer to
"which files must sit beside this artifact for it to run", emitted as
`stage_file` edges at `src/build/ninja_backend.cppm:2177-2183` — the mechanism
that puts DLLs next to a PE executable, which has no `RPATH` at all. A host
tool's store entry needs the same list. The store key must fold in the staged
set, so an entry published by an older engine that staged nothing is a miss
rather than a silently incomplete hit.

Two constraints on how it is staged, both of which a "copy the `.so` next to the
exe" reading would get wrong:

- **Preserve the sub-build's relative layout, do not flatten to `$ORIGIN`.**
  The tool's `RPATH` was written by the linker against the sub-build's
  directory shape. In the reported case that shape happened to make `$ORIGIN`
  sufficient, but a target laid out as `bin/` + `lib/` carries
  `$ORIGIN/../lib`, and flattening would leave a correct-looking entry that
  still does not resolve. Publishing the relative paths the sub-build produced
  is layout-independent; "beside the exe" is not.
- **Stage the transitive closure, not the direct dependencies.** A staged `.so`
  has its own `DT_NEEDED`. Stopping at depth one produces exactly the failure
  this item is about, one level down, and D6 is what would catch it.

**D6 — the tool's own closure is validated before the entry is published.** The
sub-build already runs the same `NinjaBackend::build` as any other build, and
therefore already runs the closure validator — but it validates against the
scratch tree, where the provider still exists. The check that matters is
against the **published** entry. One re-validation after the rename, with the
store entry's directory as the artifact's own directory, converts "the tool
will fail to start when the consumer runs it" into a build error at the point
where the tool is built.

**D7 — decide, explicitly, whether the SubOS farm belongs in a host tool's
`RPATH`.** The issue proposes removing it. That is the right instinct and the
wrong altitude: the farm is what makes `-lGL` work at all, and removing it from
one artifact class trades a silent wrong answer for a hard failure in a
different set of cases. D8 below is the general form, and it makes the farm
safe to keep by making its contribution visible. If, after D8, a host tool
still has no legitimate use for the farm, removing it becomes a small,
separately justified change rather than a workaround.

### 3.4 Criterion

Two link units in one fixture: a host tool with a `kind = "shared"` dependency,
and the same tool with a `kind = "lib"` dependency. Assert on the **store entry
contents**, not on whether the tool ran:

- the shared case publishes a store entry containing the executable *and* the
  `.so`, and `ldd`-equivalent resolution of the published executable names the
  staged file and no path under the SubOS farm;
- the static case publishes an entry containing exactly one file — the
  denominator, so that "we stage everything" cannot pass by staging nothing;
- with the provider removed from the SubOS entirely, both cases still behave
  the same. This is the step that fails today, and it is the one that a
  developer machine cannot run without being made clean first.

---

## 4. #537 — the guard is satisfiability-based and therefore cannot fire

The analysis is in `.agents/docs/2026-08-30-issues-532-533-534-analysis.md` §3
and is not repeated. What follows is the design it deferred.

### 4.1 The predicate is already available

The issue offered two directions and judged the cheaper one (warn on a host
search path in `ldflags`) as not resting on facts, and the better one (compare
what the linker resolved against what the loader will resolve) as requiring
information from both sides. The second is closer to hand than it appeared:

- `runtime_search_closure` returns an **ordered, provenance-tagged** directory
  list (`src/build/plan.cppm:728`, `modules/platform/src/runtime_search.cppm:45`).
- `resolve_runtime_closure` returns, per `DT_NEEDED`, the **file** that
  satisfied it (`src/runtime/elf.cppm:67`, `objects`).
- `carries_foreign_link_inputs` already recognises all three spellings of a
  link-time search path — `-Ldir`, `-Wl,-Ldir`, `-Wl,--library-path`, plus the
  MSVC form (`src/build/linkage_form.cppm:186-198`).

Joining the first two gives the provenance of every runtime provider. That is
the whole predicate.

### 4.2 Design

**D8 — record, per `DT_NEEDED`, the provenance of the directory that satisfied
it, and report the two cases that provenance makes provable.**

*Case 1 — satisfied by the farm, declared by nobody.* A SONAME whose runtime
provider has `Origin::SubosFarm` and which no resolved dependency in the graph
provides is, by construction, "supplied by this machine". Report it as
degraded, naming the SONAME, the file, and the fact that a clean environment
will not have it. This is the one rule that covers #535's silent wrong
`libexpat` and #537's silent wrong `libgbm`.

**`Origin::SubosFarm` ALONE IS NOT THE PREDICATE, and taking it as one would
make this rule fire on correct builds.** The farm is a symlink view *of
installed packages*, verified:

```
$ ls -l ~/.mcpp/registry/subos/default/lib/
crt1.o -> /home/…/.mcpp/registry/data/xpkgs/xim-x-glibc/2.44/lib64/crt1.o
```

A declared dependency's library is therefore routinely reached *through* the
farm rather than through a `Package`-origin directory. #532's own measurement
is the counter-example that matters: declaring four packages resolves an
eleven-entry closure whose `libexpat`, `libffi` and `libGLdispatch` no manifest
names, and all of those arrive by way of the farm while being perfectly
legitimate transitive contents of declared packages. A rule keyed on the origin
tag would report every one of them.

The predicate is one step further: **canonicalise the resolved file through the
symlink, read the owning `xim-x-<pkg>/<version>` segment, and ask whether that
package is in the resolved graph.** Farm-origin plus an owner in the graph is
correct and silent; farm-origin plus an owner outside it — or a farm entry that
is not a symlink into the store at all — is the reportable case. This is the
difference between a rule that means "declared" and a rule that means
"reachable", and it is the whole content of D8.

*Case 2 — a link-time search path that is not in the runtime closure at all.*
The set of `-L` directories, minus the runtime search closure, is exactly the
set of directories that can contribute ABI and can never contribute a runtime
provider. If a `DT_NEEDED` was produced by a `-l` whose only possible source is
in that set, then the link-time provider and the run-time provider are
different files as a matter of construction — not as a guess. Report it, name
both files, and name the flag line that caused it.

Case 2 subsumes the cheap direction the issue proposed and is strictly better
than it: it is silent when the user's `-L` names a directory that is also on
the runtime path (where nothing is wrong), and it fires with two file paths
rather than a heuristic when it is.

**Severity and wording.** Degraded, promoted to an error by `--strict`, per the
§1.2 rule: the artifact builds and runs, so the developer's choice stands. The
sentence that makes it actionable is not "do not do this" but the route back
into the ecosystem — the vocabulary in §1.2, used verbatim here so a developer
meets one sentence and not three. Case 1 will fire on real, currently-green
graphics builds; that is the intended reach of the rule and the reason it does
not refuse.

**D9 — the provenance goes into the published record, and into the
distribution statement.** `resolution.json` already publishes `search` with
provenance and `requirements`/`providers`; the resolved provider's origin per
SONAME belongs beside them. This makes "this artifact's closure is satisfied
entirely by declared packages" an assertion a CI job can make, which is the
third argument in the issue — today a `--list` on a developer machine cannot
separate "declared" from "installed here".

**The second consumer is the one that makes this more than a warning.**
`mcpp.pack.host_requirements` exists to answer "what must the TARGET machine
supply", precisely because a vendor driver cannot be bundled; its header states
that two consumers must produce the same list from the same plan — `mcpp pack`
writes it beside the artifact and `mcpp publish` projects it into an xpkg
descriptor's `[runtime].requirements` — and that "deriving it twice is how the
two drift". A host-supplied or farm-supplied provider **is** a host
requirement. So D9 must feed that existing list rather than stand up a parallel
one, and the payoff is concrete: a project that opts into a host library gets a
warning at build time and a truthful `host requirements` entry at pack time,
instead of a bundle that quietly omits the library and fails on the user's
machine. That is the §1.2 policy made operational at the distribution layer —
the developer may depend on the host, and what they must then guarantee is
written down for them rather than left implicit.

### 4.3 Criterion

- A fixture linking a library present **only** in the SubOS: Case 1 fires, and
  the record names `subos_farm` as the provider origin.
- The same fixture after the library is declared as a dependency: Case 1 is
  silent, and the record names `package`. Both halves are needed; the first
  alone is satisfied by a rule that always fires.
- A fixture with `-L` naming a directory that *is* on the runtime path: Case 2
  is silent. This is the assertion that keeps Case 2 from degenerating into the
  path-substring heuristic it replaced.

---

## 5. #527 Bug 2 and RFC 3 — workspace configuration inheritance

### 5.1 What is inherited today

Two sites, one for "the command was issued at the workspace root and `-p`
selected a member" (`src/build/prepare.cppm:1119-1138`) and one for "the
command was issued inside a member directory" (`:1153-1163`). Both merge four
things:

| key | discipline |
|---|---|
| `[workspace.dependencies]` | **explicit opt-in** — `x.workspace = true` per dependency |
| `[toolchain]` | implicit, member wins if it declares any |
| `[target.<triple>]` | implicit, per triple, member wins per triple |
| `[indices]` | implicit, member wins if it declares any |

Four keys, three disciplines, and the disciplines are not stated anywhere. That
is the actual subject of RFC 3, and it matters more than the list of keys: the
next key added will pick a discipline by whichever neighbour its author read.

### 5.2 What is not inherited, measured

Reproduction: a rooted workspace whose root declares
`[build] cxxflags = ["-DFROM_WORKSPACE_ROOT=1"]`, and a member whose source
`#error`s if the macro is absent.

```
$ mcpp build -p child
   Compiling child v0.1.0 (.)
error: build failed
failed: obj/main.cpp.ddi
  child/src/main.cpp:2:2: error: #error "workspace [build] did not reach the member"
```

Bug 2 is real and reproduces in one command. Beyond `buildConfig`, neither site
merges `package` metadata (`standard`, `version`, `license`, `edition`),
`profiles`, `featuresMap`, `resources`, `runtimeConfig`, `xlings`, or
`conditionalConfigs`.

Two further measurements about the workspace model, for completeness:

- **Each member has its own `target/` and its own `compile_commands.json`.**
  The `WorkspaceConfig` doc comment (`modules/manifest/src/types.cppm:919-926`)
  states that members "share a unified lock file, target directory". Measured,
  they do not share a target directory. A comment that describes a property the
  code does not have is a promise, and this one should be corrected in the same
  change that settles the inheritance model.
- **A rooted workspace's bare `mcpp build` builds the root package only** and
  silently ignores every member, while a virtual workspace's bare `mcpp build`
  fans out over all of them (`src/cli/cmd_build.cppm:40-42`). This matches
  cargo's default-members semantics and is a defensible decision, not a defect
  — but it is undocumented, and the divergence between the two workspace forms
  is exactly the kind of thing a `[workspace]` specification exists to state.

### 5.3 RFC 3's stated motivation does not hold

The RFC's premise is that a member at `-std=c++23` importing a member's C++26
BMI produces a hard compiler refusal, and that `[workspace.package] standard`
prevents it. Measured, that drift cannot occur:

```
libm/mcpp.toml   standard = 26     (kind = "lib")
appm/mcpp.toml   standard = 23     ([dependencies] libm = { path = "../libm" })

$ mcpp build -p appm
    Finished dev [unoptimized + debuginfo] in 0.08s

$ compile_commands.json
main.cpp     ['-std=c++23']
libm.cppm    ['-std=c++23']        ← the dependency's declared 26 is discarded
```

The standard is graph-global, taken from the root package, and this is correct:
cross-standard BMIs are hard-incompatible, so a single value per graph is
physics rather than a simplification. It is stated as such in
`.agents/docs/2026-07-31-cpp20-standard-support-design.md` §4.3, whose only
reader is `src/build/plan.cppm:1079`.

**The defect underneath is the opposite of the one reported.** A package
declares `standard = 26`, mcpp parses it, and no decision reads it. A package
that declares 26 because it *requires* 26 is compiled at whatever the consumer
says, and fails — if it fails at all — with a compiler error inside a
dependency's translation unit that names neither package nor the mechanism.
This is Family A in its purest form: the answer is parsed, and it is not wired
to a decision.

### 5.4 The precondition was already specified, and it is the same one

`2026-07-31-cpp20-standard-support-design.md` §9-Q3 declined to add a
dependency floor check and recorded exactly why:

> The default and an explicit declaration are indistinguishable —
> `xpkg.cppm:1070` and the toml parser both write `"c++23"` when the key is
> absent, so the check would judge the entire mcpp-index ecosystem as
> "requires C++23".

Verified on `origin/main`: `modules/manifest/src/xpkg.cppm:1173` assigns
`"c++23"` unconditionally, and `modules/manifest/src/toml.cppm:294` leaves the
struct default `"c++23"` in place when the key is absent.

**The same indistinguishability blocks RFC 3 itself, and this is the finding
that connects the two.** Inheritance means "use the workspace value when the
member did not say". Today "the member did not say" and "the member said
c++23" are the same bytes. `[workspace.package] standard = 26` is therefore not
implementable — it would either silently override a member that deliberately
pinned 23, or silently do nothing.

So the precondition §9-Q3 wrote down for a future floor check is the *same*
precondition RFC 3 needs, and doing it once serves both:

> the field goes on `Manifest::package` as `std::optional<std::string>`, and
> **both** parse paths fill it, or it is the same decision derived in two
> places again.

### 5.5 Design

**D10 — declaredness before inheritance.** Make every workspace-inheritable
key optional-typed at the manifest layer, filled by both parse paths, with the
default applied at one point after inheritance rather than at parse time. For
`standard` this is the field §9-Q3 already specified. For `BuildConfig` the
scalar fields with meaningful defaults (`optLevel`, `cStandard`, `linkage`,
`bmiSchedule`, `defaultProfile`) need the same treatment; the vector fields do
not, because empty is unambiguous for them.

This is the load-bearing change. Every other item in this section is
mechanical once it exists, and none of them is correct without it.

**D11 — one stated merge discipline, with named exceptions.** The default is
**the member wins when it declared the key; otherwise the workspace value
applies**, which is what `toolchain`, `[target.*]` and `[indices]` already do
and what a reader will expect from all four. `[workspace.dependencies]`
keeps its explicit `x.workspace = true` opt-in, and the reason is worth writing
down rather than leaving as an accident: a dependency is an edge in the
resolution graph, and inheriting edges implicitly would change what a member
resolves without the member's manifest mentioning it. Vectors
(`cxxflags`, `ldflags`, `dialect_cxxflags`) **append**, workspace first, so a
member can add without having to restate; a member that needs to *not* have a
workspace flag is a signal that the flag was declared at the wrong altitude.

**Not every `[build]` key should be inheritable, and the exclusions must be
named rather than left to whoever writes the merge.** `allow_host_libs` is a
policy escape hatch that disables a correctness gate; a workspace root that
sets it once would silently disable that gate for every member, including
members added later by someone who never read the root manifest. The same
argument applies to any future key whose effect is "turn a refusal off".
Inheritable keys are those that describe *how to build*; keys that describe
*which safety check not to run* stay per-package, where the person turning them
off is the person who owns the artifact.

**D12 — `[workspace.package]`, `[workspace.build]`, `[workspace.target.<triple>]`.**
With D10 and D11 in place these are three table names and one merge function,
not three features. `WorkspaceConfig` currently holds `members`, `exclude`,
`dependencies` and `present` (`modules/manifest/src/types.cppm:927`); the three
new tables are the same shape as the member-side tables they mirror. Both
inheritance sites must call the one merge function — today they are two copies
of the same four merges, and a fifth key added to one of them is a defect that
compiles.

**D13 — with declaredness available, add the floor check §9-Q3 deferred.** A
dependency that *declared* a standard higher than the resolved graph standard
is reported before compiling, naming both packages, the two values, and the fix
(raise the root's `standard`, or `[workspace.package] standard`). A dependency
that did not declare one is silent, which is the whole point of D10. This
retires the "diagnostic enhancement" stopgap in the cpp20 design doc's §4.3 — appending a hint to a
failing dependency compile — with the check that stopgap was standing in for.

**It ships degraded first, not as an error, and the reason is not caution for
its own sake.** The condition is not a proven failure: a package declaring
`standard = 26` compiles perfectly well at 23 whenever it happens not to use a
C++26 construct, and today that is a working, green configuration for anyone
who wrote the key aspirationally. Making it an error on first release converts
green builds to red with no defect behind them. Degraded, promoted by
`--strict`, matches how the closure rules and `symbol_provision` were rolled
out. This differs from D14, where the compile provably cannot succeed, and that
difference is exactly why the two carry different severities — and it is the
§1.2 rule applied: it builds and runs, so it is warned about.

**D13a — and the declaredness bit is not sufficient for index packages. This is
the review's sharpest finding, and it is measurable.** §9-Q3's trap was stated
as a parser problem: both parse paths write `"c++23"` when the key is absent,
so absent and declared are indistinguishable. D10 fixes that for `mcpp.toml`.
It does **not** fix it for the index, because index descriptors declare the
value explicitly. Counted over the local registry (2727 descriptors, 774 with
an mcpp segment):

```
  678  language     = "c++23"
   60  language   = "c++23"
   36  language     = "c++20"
    8  language = "c++23"
  ────
  782  declarations — every descriptor that has an mcpp segment declares one

  756 of those 774 also declare `import_std = false`
```

`language` is the descriptor key that feeds `Package::standard`
(`modules/manifest/src/xpkg.cppm:1202-1203`). So **every package in the index
"declares" a C++ standard**, and for the 98 % that are C libraries with
`import_std = false` the declaration is boilerplate rather than a requirement.
A floor check keyed on declaredness would, for a root at c++20, fire against
essentially the entire index — which is precisely the outcome §9-Q3 refused,
arrived at through a different door.

So D13 is scoped to **manifests the project author controls**: the root
package, workspace members, and `path` dependencies. There, a declared
`standard` was typed by the person reading the diagnostic. Index packages are
out of scope until the ecosystem has a key that means "requires at least",
which is a descriptor-schema change gated on the index floor and is not part of
this work. Recording the boundary matters more than the check: the check
without it is a mass false positive, and the shape of that false positive is
invisible from inside mcpp's own repository.

### 5.6 Criterion

- A member that declares nothing inherits `[workspace.build] cxxflags`; a
  member that declares its own `cxxflags` gets both, workspace first. Assert on
  the **compile command in `compile_commands.json`**, not on build success.
- A member that declares `standard = 23` under a workspace declaring 26 is
  compiled at 23 — the assertion that fails if declaredness was faked with a
  sentinel value.
- A member that declares nothing under a workspace declaring 26 is compiled at
  26.
- The floor check fires for a dependency that **declared** a higher standard,
  and does not fire for the whole existing index, whose packages declare
  nothing and are defaulted. The second half is the denominator, and it is the
  assertion that would have caught the trap §9-Q3 recorded.
- Both inheritance sites are exercised: the command issued at the workspace
  root with `-p`, and the command issued inside the member directory. Two sites
  means an assertion on one of them proves nothing about the other.

---

## 6. #527 RFC 2 — dialect flags and the `import std` BMI

### 6.1 Measured

```toml
[build]
cxxflags = ["-fno-exceptions"]
```

```
$ mcpp build
   Compiling noexc v0.1.0 (.)
error: build failed
failed: obj/main.o
std: error: language dialect differs 'C++23', expected 'C++23/no-exceptions'
std: error: failed to read compiled module: Bad file data
std: note: compiled module file is 'gcm.cache/std.gcm'
```

```toml
[build]
dialect_cxxflags = ["-fno-exceptions"]
```

```
$ mcpp build
    Finished dev [unoptimized + debuginfo] in 0.61s
```

The maintainer's A4/A5 is correct in full: the key exists and it works.

### 6.2 The mechanism is complete

`dialect_flags()` (`modules/manifest/src/types.cppm:1217`) returns explicit
`dialect_cxxflags` plus auto-promoted known flags; `prepare_build` appends the
result to the standard flag to form one `stdFlagAndDialect` string
(`src/build/prepare.cppm:7612-7636`) that is shared by the p1689 scan and by
`stdmod::ensure_built`, so scan-time, prebuild-time and compile-time dialect
provably agree. Nothing about the plumbing is missing.

What is missing is that `-fno-exceptions` and `-fno-rtti` are deliberately
excluded from the auto-promotion list, with the reason recorded in the code
(`modules/manifest/src/types.cppm:1200-1204`): dependencies may assume
exceptions are available. That reason is correct and should stand.

### 6.3 Why auto-promotion is the wrong fix

Promotion makes a per-unit flag graph-global and silently changes how every
dependency is compiled. A dependency that uses `try`/`catch` then fails to
build, and the user who wrote one flag in their own `[build]` table has no
reason to look there. The split in the list is not arbitrary and should be
stated as a rule rather than left as a list:

> A flag is auto-promoted when a graph that mixes it is ill-formed anyway —
> `-freflection`, `-fchar8_t`, `-D_GLIBCXX_USE_CXX11_ABI=` change what the
> standard library headers *declare*, so no dependency can hold a coherent
> different opinion. A flag is not auto-promoted when a dependency can
> legitimately disagree: `-fno-exceptions` and `-fno-rtti` remove a language
> facility that a dependency may use and that the consumer cannot decide on its
> behalf.

### 6.4 Design

**D14 — refuse, and name the key.** The condition is provable, not heuristic:
the graph imports `std` (mcpp already computes this as `needsStdModule`,
`src/build/prepare.cppm:7682`, and already gates two refusals on it at `:7828`
and `:7914`), and `[build] cxxflags` contains a
dialect-class flag that is not in `dialect_flags()`. Under those two facts the
compile *will* fail with the message above. Refuse before compiling, with the
line the user can paste:

```
error: `-fno-exceptions` in [build] cxxflags changes the language dialect, but
       the `import std` BMI is precompiled without it, so every importing TU
       will fail with "language dialect differs".
       Fix: move it to `[build] dialect_cxxflags`, which is applied to the std
       BMI prebuild, the module scan and every TU in the graph — including
       dependencies, which is what makes the graph coherent.

         [build]
         dialect_cxxflags = ["-fno-exceptions"]
```

The recognition list for this check is the one `is_dialect_flag` already
maintains, extended with the two flags that are recognised-but-not-promoted.
One list, two consumers, so the day a third dialect flag is added it cannot be
added to only one of them.

`-fno-rtti` behaves identically and is therefore in scope, measured rather than
assumed:

```
std: error: language dialect differs 'C++23', expected 'C++23/no-rtti'
```

**The check reads the EFFECTIVE flags, not `[build] cxxflags`.** The same flag
can arrive from `[profile.<name>] cxxflags`, from
`[target.<triple>.build] cxxflags`, or from a `cfg(...)` conditional block, and
all of them reach the compile line while none of them reaches the std BMI
prebuild. A check that reads only the one table is silent on three spellings of
the same mistake — which is the shape this whole document keeps finding, and it
would be careless to reproduce it in the fix for it.

### 6.5 Criterion

- `cxxflags = ["-fno-exceptions"]` plus `import std` refuses **before** any
  compile edge runs, and the message contains the string `dialect_cxxflags`.
- `dialect_cxxflags = ["-fno-exceptions"]` plus `import std` builds.
- `cxxflags = ["-fno-rtti"]` refuses on the same terms, so the list is exercised
  and not just its first entry.
- `cxxflags = ["-D_GLIBCXX_USE_CXX11_ABI=0"]` plus `import std` **builds**, and
  builds silently. This is the control that keeps the check from swallowing the
  auto-promotion path: an auto-promoted flag is already in `dialect_flags()`, so
  a check that fires here is testing "is this flag dialect-class" instead of
  "did this flag reach the BMI". Verified as green today.
- `cxxflags = ["-fno-exceptions"]` **without** `import std` anywhere in the
  graph still builds. This is the denominator: a check that refuses in both
  cases has stopped testing the condition it claims to test.
- The same flag placed in `[profile.dev] cxxflags` refuses too — the assertion
  that the check reads effective flags.

---

## 7. #527 Bug 1 — a crash where a refusal belonged

Reproduced verbatim:

```
$ cat mcpp.toml
[package] name = "demo" ... 
[toolchain] linux = "system"
$ ls build.mcpp
build.mcpp
$ mcpp build
  build.mcpp compiling
error: build.mcpp failed to compile (exit 127):
posix_spawnp('') failed (error 2): No such file or directory
```

The reporter's source-level diagnosis is exactly right, at shifted line
numbers. `explicit_compiler` is left empty by the `system` branch
(`src/build/prepare.cppm:2160-2161`), and the native branch of
`host_tc_for_build_program` returns it unchanged
(`src/build/prepare.cppm:2825-2826`).

The interesting part is what sits eight lines further down. The **cross** branch
already refuses this configuration, with a good message:

```
build.mcpp under a cross --target needs a resolvable host toolchain —
set one via [toolchain] or `mcpp toolchain default`
```

So a classified refusal for "no host toolchain is resolvable" is written and
correct. It is guarded by `!overrides.target_triple.empty()`, and the failing
case is the native one — which is why the native path reaches `posix_spawnp`
with nothing at all. Reading it as "the refusal is unreachable" was this
document's first conclusion, and §1.2 corrects it: the cross branch refuses
because nothing was resolved, whereas on the native path something *was*
resolved and simply was not handed over.

**D15 — refuse `[toolchain] system`, with `msvc@system` the one exception.**
This reverses what an earlier draft of this document proposed, and the reversal
comes from §1.2's corrected form: the boundary is per axis, and the toolchain is
not the axis a project may take from the host.

The crash itself is real and its diagnosis stands. `explicit_compiler` is left
empty by the `system` branch — it has nothing to assign until `detect` finds the
PATH compiler and stores the absolute path in `tc->binaryPath` — and the native
branch of `host_tc_for_build_program` returned the local variable, handing `""`
to `posix_spawnp`. The main build read the compiler from `tc` and worked, which
is why only `build.mcpp` died.

What changed is the conclusion drawn from it. Filling the variable makes the
escape hatch consistent, and a consistent unsupported configuration is still an
unsupported configuration. **A refusal that arrives as a crash three layers down
is not a policy; it is a bug wearing one.** So the fix is the refusal the
configuration always warranted, raised where the specification is read:

```
error: [toolchain] linux = "system" is not supported: mcpp builds only with
       toolchains it manages.
       A compiler taken from PATH cannot be identified or reproduced, so
       `import std` availability, the runtime closure and "the same build on
       another machine" all stop being things mcpp can promise.
       Name one instead — mcpp installs it on first use:

         [toolchain]
         linux = "gcc@16.1.0"

       or set a machine default with `mcpp toolchain default gcc@16.1.0`, and
       see `mcpp toolchain list` for what is available.
       (On Windows, `msvc@system` is different and remains supported: it names
       a family whose installation mcpp locates.)
       Host LIBRARIES are a separate question and are not refused — a project
       may link them and owns the result.
```

The last two lines are load-bearing rather than courtesy. A Windows user
reading a blanket "system is not supported" would reasonably conclude
`msvc@system` had been removed, and a user who came here from a host-library
warning would reasonably conclude the two axes had merged. Both are wrong, and
neither is inferable from the refusal without saying so.

**That the configuration works today is not an argument against refusing it.**
Measured, `[toolchain] system` compiles a project using `import std` in 2.04 s
on a host with a new enough compiler. The measurement was right and the
inference from it was wrong: what mcpp promises is not "this compiled here",
it is "this compiles the same way elsewhere", and a PATH compiler cannot
support that sentence no matter how well it performs on one machine.

**The cross-target branch is unchanged.** There `explicit_compiler` is empty for
a different reason — no host toolchain was resolved at all — and its classified
`refusal::Code::HostToolToolchain` remains the right answer.

**Three existing tests referenced the escape hatch, and each needed a different
answer.** Recorded because two of them would have gone on passing:

| test | what happened | what it needed |
|---|---|---|
| `14_toolchain_fallback` | asserted only that `system` did NOT produce "no toolchain configured" — a predicate any other error also satisfies. It passed while its stated intent inverted. | assert the refusal on its own terms, both halves |
| `293_…_name_one_os` | used `system` to point a Linux compiler at a Windows target; the refusal fires first, so it began taking its skip branch — which its own header says must be earned or the test cannot see a revert | accept the refusal as a PASS branch with its reason: the door it guarded is now closed entirely |
| `105_asm_sources_nasm` | genuinely unaffected — its broken-`MCPP_HOME` bootstrap error still fires first | nothing; verified rather than assumed |

A negative-only assertion cannot distinguish "it worked" from "it failed
differently", and a skip cannot distinguish "not applicable here" from "this
test stopped testing". Both traps were live in this change.

**Criterion.** Five assertions, and the last is the one that keeps the fix
honest:

- `[toolchain] system` is refused with no `build.mcpp`, and with one, and the
  refusal reaches the user before the build program is compiled — no
  `posix_spawnp`, no `build.mcpp compiling` line.
- `MCPP_TOOLCHAIN=system` is refused identically: one policy cannot have two
  answers depending on which channel stated it.
- The message names what to write instead, the command that lists the choices,
  the `msvc@system` exception, and the library axis.
- A project with no `[toolchain]` at all **still builds**, and does not see the
  refusal — the denominator, without which "refuse everything" satisfies every
  assertion above.

## 8. #527 RFC 4 — the aggregate `compile_commands.json`

Measured: each member writes its own `compile_commands.json` in its own
directory; there is no root aggregate, and `compile_commands.cppm` has no
workspace-aware path.

The request is well-founded — a language server rooted at the workspace
directory finds nothing — and the maintainer has deferred it to a later
release. Two notes for whoever builds it:

- The aggregate must be written by the `--workspace` fan-out, after the last
  member, from the per-member files. Building it inside `run_configure_plan`
  would make each member's write a partial overwrite of the root file, and the
  last member to finish would win.
- Entries from different members can name the same `file` under different
  flags, which is legal in the format and which some consumers resolve by
  taking the first match. The aggregation order should be the declared member
  order, so the result is at least deterministic.

Deferred here as well; recorded so the later work does not start from a blank
page.

---

## 9. Not recommended

- **Auto-promoting `-fno-exceptions` / `-fno-rtti` to dialect flags** (#527 RFC
  2 as literally written). §6.3. The existing exclusion is correct; the defect
  is the silence, and D14 removes it without changing what the flags mean.
- **`[workspace.package] standard` before the declaredness bit** (#527 RFC 3 as
  literally written). §5.4. It is not implementable — "the member did not say"
  and "the member said c++23" are the same bytes today, and shipping the table
  without D10 produces an inheritance rule that silently overrides deliberate
  member pins.
- **Filling in the host compiler so `[toolchain] system` works consistently.**
  §7. An earlier draft of this document proposed exactly that, on the ground
  that the configuration builds and runs. It does; that is not the question.
  The toolchain is mcpp's own contract and is refused, so the reporter's
  `explicit_compiler.empty() ? tc->binaryPath : …` patch is NOT adopted.
- **Any of this being read as movement on RFC 1.** `sysroot = "system"`,
  suppressing the private `PT_INTERP`, and not injecting RPATH remain out of
  scope and untouched. D15 moves in the opposite direction from RFC 1: it
  narrows what the host may supply rather than widening it.
- **Refusing a host LIBRARY.** §1.2. That axis is the project's own, and the
  answer there stays a warning that names the mcpp-index route.
- **Removing the SubOS farm from host tool `RPATH`s as the fix for #535.**
  §3.3/D7. It trades a silent wrong answer for a hard failure in a different
  set of cases, and D8 makes the farm's contribution visible without that
  trade.
- **Warning on host search paths in `ldflags` as the fix for #537.** §4.2. Case
  2 is the same idea resting on two file paths instead of a path substring, and
  it is silent in the case where the substring rule produces noise.
- **`Origin::SubosFarm` as the "undeclared provider" predicate** (D8 as first
  drafted). §4.2. The farm is a symlink view of installed packages, so a
  declared dependency's library routinely resolves through it; the predicate
  has to canonicalise and ask who owns the file.
- **D3 before D3a.** §2.5. Extending the fast path to `-p` while the staleness
  sweep is blind to `path` dependencies would promote a narrow existing hole
  into the normal workspace case.
- **A blanket refusal of `[toolchain] system`.** §7. Measured, that
  configuration builds a project with `import std` today; only the `build.mcpp`
  path is broken, and only that path should be refused.

---

## 10. Sequencing

Ordered by what unblocks what, not by severity.

| step | items | unblocks |
|---|---|---|
| 1 | D10 (declaredness on inheritable keys, both parse paths) | D12, D13; nothing in §5 is correct without it |
| 2 | D1, D1a, D2 (records move to the surviving sidecar; key covers farm and policy; prune on disk absence, not on plan absence) | #529's dominant cost; independent of everything else. D1 without D1a trades slow-and-correct for fast-and-stale; D1 without D2 leaves the developer loop paying |
| 3 | D11, D12 (stated merge discipline; the three workspace tables) | #527 Bug 2, RFC 3 |
| 4 | D15 (refuse `[toolchain] system`; `msvc@system` excepted) | #527 Bug 1. One refusal plus three existing tests that referenced the escape hatch |
| 5 | D14 (dialect flag refusal), incl. the MSVC spellings or a stated GNU-only scope | #527 RFC 2 |
| 6 | D3a (staleness sweep covers `path` dependency source roots) | a defect on its own; D3's precondition |
| 7 | D8, D9 (provenance per `DT_NEEDED`, published, feeding `host_requirements`) | #537, the safe half of #535, and truthful `mcpp pack` host requirements |
| 8 | D5, D6 (tool store publishes a directory; closure validated after publish) | #535 |
| 9 | D3 (fast path honours `-p`), D13 + D13a (standard floor check, degraded, author-owned manifests only) | after their preconditions land |
| 10 | D4 (test fast path), D7 (farm in tool `RPATH`), RFC 4 | re-measure before designing |

Steps 2, 4, 5 and 6 are independent of every other step and of each other; any
can go first. Step 2 is the one with a user-visible number attached (§11.1),
and step 4 is the cheapest.

**One ordering constraint that is not about dependencies.** Steps 4, 7 and 9
each add a diagnostic channel that fires on builds which are green today (§12.3).
They should not land in the same release. A warning column that grows by three
in one version is read as noise, and the one that matters most — D8's, which is
a genuine correctness statement about what an artifact will load — is the one
that would be discounted.

---

## 11. What changes for a user

Every example below is the behaviour after the corresponding step, written
against the reproductions in this document.

### 11.1 The edit-test loop stops paying for answers it already has (D1, D1a, D2)

Nothing in the interface changes. The numbers do. On the measured fixture — ten
link units, 11.5 MB each, everything warm:

```
                     before        after (projected)
mcpp build -p app     0.70 s        0.40 s
mcpp test  -p app     3.15 s        0.60 s
mcpp test  -p app     1.95 s        0.60 s
```

The "after" column is the measured wall clock minus the three stage timings,
which is what the memo is expected to remove; it is a projection and is the
number the criterion in §2.6 should be held to, not a promise.

One behaviour does change and is worth calling out in release notes: a verdict
that was previously recomputed on every invocation is now read from a record,
so `resolution.json` and the sidecar become load-bearing. Deleting the output
directory remains the way to force a full re-derivation, and D1a's key is what
makes that unnecessary in the cases users actually hit (`xlings install`,
toggling `MCPP_ALLOW_HOST_LIBS`).

### 11.2 A workspace declares shared settings once (D10, D11, D12)

Before — every member restates everything, and the root's `[build]` is inert:

```toml
# mcpp.toml (workspace root) — the [build] table here does nothing today
[workspace]
members = ["apps/compositor", "packages/render", "packages/proto"]

[build]
cxxflags = ["-Wall", "-Wextra"]     # silently ignored by every member
```

```toml
# apps/compositor/mcpp.toml — and the same six lines in every other member
[package]
name     = "compositor"
version  = "0.1.0"
standard = 26

[build]
cxxflags = ["-Wall", "-Wextra"]
```

After:

```toml
# mcpp.toml (workspace root)
[workspace]
members = ["apps/compositor", "packages/render", "packages/proto"]

[workspace.package]
standard = 26
version  = "0.1.0"
license  = "Apache-2.0"

[workspace.build]
cxxflags         = ["-Wall", "-Wextra"]
dialect_cxxflags = ["-fno-exceptions"]

[workspace.target.x86_64-linux-gnu]
toolchain = "gcc@16.1.0"
```

```toml
# apps/compositor/mcpp.toml
[package]
name = "compositor"
# standard, version, license inherited; [build] inherited
```

A member that wants something different says so, and the member wins:

```toml
[package]
name     = "legacy-shim"
standard = 23            # deliberate, and honoured — this is what D10 buys
```

`mcpp build -p legacy-shim` compiles it at 23; every other member at 26. Today
both spellings produce 23 for the whole graph and the workspace table is inert.

**Migration.** Existing manifests are unaffected: `[workspace.*]` tables are
absent, so nothing is inherited and every member keeps its own values. There is
no index floor concern either — `[workspace.*]` appears only in a workspace
root manifest, which is never published to the index, so no released mcpp is
ever asked to read a key it does not know.

### 11.3 A misplaced dialect flag says where it belongs (D14)

Before:

```
$ mcpp build
   Compiling noexc v0.1.0 (.)
error: build failed
failed: obj/main.o
std: error: language dialect differs 'C++23', expected 'C++23/no-exceptions'
std: error: failed to read compiled module: Bad file data
std: note: compiled module file is 'gcm.cache/std.gcm'
```

After:

```
$ mcpp build
error: `-fno-exceptions` in [build] cxxflags changes the language dialect, but
       the `import std` BMI is precompiled without it, so every importing TU
       will fail with "language dialect differs".
       Fix: move it to `[build] dialect_cxxflags`.

         [build]
         dialect_cxxflags = ["-fno-exceptions"]
```

No project that builds today starts failing: the refusal fires only where the
compile already fails.

### 11.4 A package that needs a newer standard says so (D13)

```
$ mcpp build
warning: dependency `render` declares `standard = 26`, and this graph is built
         at c++23 (from [package] standard of `compositor`). A C++ module graph
         has one standard; the dependency's declaration is not applied.
         Fix: raise the consumer's standard, or set it once for the workspace:

           [workspace.package]
           standard = 26
```

Degraded, so the build continues; `--strict` promotes it. Packages that never
declared a standard are silent, which is every package in the index today.

### 11.5 "This closure is satisfied by declared packages" becomes checkable (D8, D9)

Before — on a machine with Mesa installed, a build that takes its ABI from
`/usr/include` and its runtime library from the SubOS says nothing at all:

```
$ mcpp build
   Compiling linkab v0.1.0 (.)
    Finished dev [unoptimized + debuginfo] in 0.05s
```

After:

```
$ mcpp build
   Compiling linkab v0.1.0 (.)
warning: build/provenance: `libgbm.so.1` is linked from
         /usr/lib/x86_64-linux-gnu/libgbm.so.1 (a -L directory that is not on
         this artifact's runtime search path) and loaded from
         ~/.mcpp/registry/subos/default/lib/libgbm.so.1 (SubOS, provided by
         xim:mesa, which no dependency declares).
         The program runs against a different build than it was compiled
         against.
note:    mcpp builds against no host library by default, so this artifact is
         not reproducible on another machine and `mcpp pack` will record
         libgbm.so.1 as a host requirement rather than bundling it.
         The supported route is to declare the provider as a dependency from
         mcpp-index. If mcpp-index does not carry it yet, contributing the
         package is the path, and the dependency then resolves the same way on
         every machine and in CI.
    Finished dev [unoptimized + debuginfo] in 0.05s
```

The build is not refused: it compiles, it runs on this machine, and the
developer said they wanted this. What changes is that the cost is stated and
the supported alternative is named.

And the record gains a field a CI job can assert on, which is what makes "zero
host-supplied libraries" a statement rather than an impression:

```json
"runtime": {
  "providers": [
    { "soname": "libgbm.so.1", "file": ".../subos/default/lib/libgbm.so.1",
      "origin": "subos_farm", "owner": "xim:mesa", "declared": false }
  ]
}
```

Expect this to fire on existing green graphics builds. That is the point, and
it is why it is degraded rather than an error on first release.

### 11.6 A host tool that needs a shared library works on a clean machine (D5, D6)

Before, on a clean runner:

```
    Building host tool wayland-scanner:wayland-scanner from wayland-scanner v1.26.0
error: dependency 'wayland': build.mcpp exited with 1 (build aborted):
  .../bin/wayland-scanner: error while loading shared libraries:
  libexpat.so.1: cannot open shared object file: No such file or directory
```

Before, on a developer machine with `xim:expat` installed: it *works*, against
a different version than the one declared.

After, both machines behave the same, and the store entry contains what the
tool needs:

```
$ ls <build-cache>/v1/tool/freedesktop/wayland-scanner@1.26.0/<hash>/bin/
wayland-scanner   libexpat.so.1
```

If the closure still cannot be satisfied, the failure moves to where the tool
is built and names it, instead of arriving as a loader error inside an
unrelated package's `build.mcpp`.

### 11.7 `toolchain = "system"` is refused, and says what to write instead (D15)

Before, adding a `build.mcpp` to a project that otherwise builds:

```
$ mcpp build
  build.mcpp compiling
error: build.mcpp failed to compile (exit 127):
posix_spawnp('') failed (error 2): No such file or directory
```

After — and with or without the `build.mcpp`, because the configuration itself
is what is refused:

```
$ mcpp build
error: [toolchain] linux = "system" is not supported: mcpp builds only with
       toolchains it manages.
       A compiler taken from PATH cannot be identified or reproduced, so
       `import std` availability, the runtime closure and "the same build on
       another machine" all stop being things mcpp can promise.
       Name one instead — mcpp installs it on first use:

         [toolchain]
         linux = "gcc@16.1.0"

       or set a machine default with `mcpp toolchain default gcc@16.1.0`, and
       see `mcpp toolchain list` for what is available.
       (On Windows, `msvc@system` is different and remains supported.)
       Host LIBRARIES are a separate question and are not refused.
```

A project that used the escape hatch has to name a toolchain, which mcpp
installs on first use. A project with a declared toolchain sees no new output
at all, and a project that links host **libraries** is untouched — that is the
other axis, and its answer is still a warning.

### 11.8 Adding a file to a `path` dependency is no longer invisible (D3a)

Before:

```
$ printf 'export module dep.third;\n...' > ../dep/src/third.cppm
$ mcpp build
    Finished dev in 0.00s        # the module was never compiled
```

After: the fast path is abandoned, the module is compiled, and the archive
gains an object. The cost is that a project with `path` dependencies stats
their source trees on every invocation, which is the same cost it already pays
for its own.

---

## 12. The plan under six other lenses

Reviewed after the design was written, deliberately from angles the design was
not written from. Findings that changed the design are marked; the rest are
constraints the implementation has to carry.

### 12.1 Cross-platform

**D8/D9 are Linux-only as designed, and that must be stated rather than
discovered.** `runtime_search_closure` guards on format: `DT_RPATH` exists on
ELF only, so "Mach-O and PE get nothing rather than a branch in every consumer"
(`src/build/plan.cppm:754-772`). The provenance join therefore has no input on
the other two platforms. This is not a defect in D8 — it is the correct scope —
but the vocabulary must not become ELF-shaped, because the question is
universal:

| platform | the same question | the mechanism that would answer it |
|---|---|---|
| Linux | which directory on the search path supplied this `DT_NEEDED` | `runtime_search_closure` + `resolve_runtime_closure` — exists |
| Windows | was this DLL found beside the exe, or in `System32` / on `PATH` | `deployFiles` already stages the beside-the-exe half; the search-order model does not exist |
| macOS | `@rpath` entry, or a system framework / the dyld shared cache | neither exists |

So D9's record field should be named for the question ("which origin supplied
this provider"), not for the ELF answer, and the two other platforms should
appear in the record as "not evaluated" rather than as absent — the same
four-valued discipline `RuntimeVerdict` already uses, where "not measured" and
"clean" must not read the same.

**D5 is a cross-platform unification, not a Linux fix, and this strengthens
it.** On PE there is no RPATH at all, so a shared dependency *must* sit beside
the executable; that is exactly what `deployFiles` already does for a normal
build. A host tool's store entry publishing only the executable is therefore
broken on Windows for a more basic reason than on Linux, where the farm can
paper over it. Reusing `deployFiles` makes one mechanism serve both.

**D14 is GNU-spelled and would be silent on MSVC.** `is_dialect_flag`'s list is
`-freflection`, `-fcontracts`, `-fchar8_t`, `-D_GLIBCXX_USE_CXX11_ABI=`
(`modules/manifest/src/types.cppm:1200-1214`). The MSVC spellings of the same
dialect axes — `/EHsc` and `/EHs-c-` for exceptions, `/GR-` for RTTI — are not
in it, and `msvc_crt_flag` is already threaded into `stdmod::ensure_built`
precisely because `cl` bakes `_MSVC_MT`/`_MSVC_MD` into the module. The same
class of mismatch exists there and the check would not see it. Either add the
MSVC spellings in the same change, or state in the code that the list is
GNU-only and why — an unstated platform gap in a table like this is how the
next reader assumes coverage.

**D10–D12 are platform-neutral**, with one interaction to specify:
`[workspace.target.<triple>]` and `[target.'cfg(...)']` conditional blocks both
target-scope configuration, and the merge order between an inherited
`[workspace.target.X]` and a member's own `cfg()` block has to be written down.
The safe default is that the member's conditional evaluates after inheritance,
so a member can always narrow.

### 12.2 Compatibility and migration

**Nothing here changes a published manifest's schema.** `[workspace.*]` appears
only in a workspace root, which is never published to the index, so no released
mcpp is asked to read a key it does not know — the failure mode where an
unknown key makes a whole manifest unloadable does not apply. `mcpp pack`'s
emitted manifest does not write `standard` at all
(`src/pack/manifest_emit.cppm`, checked), so D10's optional-ness does not leak
into published descriptors either.

**D13's blast radius is measured, and it is the finding that scoped it.** See
§5.5/D13a: every index descriptor with an mcpp segment declares `language`, so
"declared" cannot mean "authored" for index packages. Scoped to author-owned
manifests, D13 fires on nothing that exists today.

**D1's record format needs a version, and half of one exists.** The sidecar
carries `schema` and `contract_hash`; adding two record kinds to it means an
older mcpp reading a newer file must ignore what it does not know rather than
discard the file — which is what a schema field is for, provided the reader is
written that way. Worth asserting, because the failure is silent: a discarded
record reads exactly like a cold cache, i.e. as a slow build rather than as an
error.

**Behaviour that changes for someone whose build is green today:** D8 Case 1
and D15's warning both add output to builds that currently print none. Neither
fails. That is the whole of the user-visible compatibility surface.

### 12.3 Usability

**The inheritance discipline diverges from cargo, deliberately.** Cargo spells
inheritance `x.workspace = true` for package fields as well as dependencies,
and #527's RFC copied that spelling. D11 makes package and build keys implicit
instead, and the argument is the RFC's own goal: if every member must opt in
per key, a member that forgets one drifts silently, which is the drift the RFC
exists to eliminate. Implicit-if-absent makes drift the thing you have to ask
for. The cost is that a cargo user's expectation is wrong once, and the answer
is documentation rather than a second mechanism.

It is also the choice that keeps the count down: mcpp already ships three
implicitly-inheriting keys (`toolchain`, `[target.*]`, `[indices]`) and one
opt-in key (`dependencies`). Making the new keys opt-in would give four keys
under two disciplines with no rule; making them implicit gives one rule and one
named exception.

**Warnings must not train people to ignore them.** D8 Case 1 will fire on
existing graphics builds, D15 on every `system`-toolchain build, D13 on
author-declared mismatches. Three new channels arriving at once is how a
warning column becomes noise. They should land in the order of §10, each with
its own release, and each must be silent on a compliant project — the last
assertion in §7's criterion exists for that reason.

**The best usability outcome in this document is not a message.** It is §11.1:
an edit-test loop that stops costing seconds per iteration. Nothing else here
is felt as often.

### 12.4 Simplicity

Counted by what an implementer touches:

| item | surface |
|---|---|
| D1, D1a, D2 | one record file, one key derivation, one prune predicate; the sync pattern already exists (`sync_resolution_verdict`) |
| D3a | one function's input set, plus one field in `.build_cache` |
| D5, D6 | reuse `deployFiles` + one post-publish validation; no new concept |
| D8, D9 | one join between two existing records; feeds an existing list (`host_requirements`) |
| D10 | optional-ness on a handful of fields, filled in **two** parse paths |
| D11, D12 | one merge function, three table names |
| D14, D15 | two diagnostics, one of which is a one-line assignment |

The two places where simplicity is at risk, both worth watching in review:

- **D10 is the only wide change.** `Package::standard` becoming optional
  touches every reader. The mitigation is that the default is applied once,
  after inheritance, so readers keep seeing a plain string — but if that
  single application point turns into three, the change has failed and should
  be reconsidered rather than finished.
- **D12's merge function must be one function called from two sites.** The two
  inheritance sites are already two copies of the same four merges. Adding
  three tables to two copies is six edits, and the sixth is the one that gets
  forgotten.

### 12.5 Architectural clarity

The plan adds **no new concepts**. Every item is an existing mechanism reaching
one more case:

| item | the mechanism it extends |
|---|---|
| D1/D2 | `sync_resolution_verdict`'s sidecar-authoritative pattern |
| D5 | `deployFiles`, the beside-the-artifact staging that PE already needs |
| D8/D9 | `search::Origin`, already tagged and already ordered |
| D9 → pack | `host_requirements`, already the two-consumer statement of host dependence |
| D11/D12 | the implicit-if-absent discipline of `toolchain`/`[target.*]`/`[indices]` |
| D13 | the floor check §9-Q3 specified and deferred |
| D14 | `is_dialect_flag`, one list with a second consumer |
| D15 | the toolchain specification is read in one place; the refusal joins the branches already there |

The one genuinely new statement is §1.2, and it is a policy rather than a
mechanism: **mcpp depends on no host; a user project may, and is warned rather
than refused whenever the result builds and runs.** Everything with a severity
in this document now derives from that sentence, which is why D15 reversed and
D13 softened during review.

### 12.6 Build and distribution

**Host dependence is a distribution property first and a build property
second, and the plan was initially written the other way round.** A warning at
build time is advice; the consequence lands at `mcpp pack`, where a
host-supplied library cannot be bundled and the honest output is a statement of
what the target machine must provide. `host_requirements` already exists for
this, already has two consumers (`mcpp pack` and `mcpp publish` →
`[runtime].requirements`), and already warns in its own header that deriving
the list twice is how the two drift. D9 feeding it is therefore not an
enhancement; deriving a second list would be the defect.

This also gives the §1.2 policy its teeth without a refusal. "You may depend on
the host and you guarantee it" becomes concrete: the guarantee is written into
the bundle, and a consumer of the package can read it.

**Reproducibility.** Two items in the plan improve it and one must be checked
not to harm it. D15's warning and D8's report both make non-reproducible inputs
visible where they were silent. D1's memoisation makes a build's *diagnostics*
depend on a cached record — if the key is wrong, two machines with identical
sources can print different warnings, which is worse than a slow build. That is
D1a, and it is why D1a is not optional.

**Offline and sandboxed builds** (#527's second comment, the distro-packaging
argument) are untouched by everything here. That request is about mcpp
resolving toolchains from the network during `%build`, which none of these
items changes; it is a separate question from host dependence and is not
addressed by this document.

---

## 13. Review record

What this document said before it was reviewed, and what the review changed.
Recorded because three of the corrections are the kind that would have shipped
as defects, and one of them found a defect that no issue reports.

| claim as first written | what measurement showed | where |
|---|---|---|
| "the record is incomplete because the last drive within an invocation replaced it" | False. `mcpp test`'s plan genuinely contains only the nine test binaries — verified against the emitted `cxx_link` edges. The real collision is **between commands**: `build` and `test` share one output directory with different link-unit sets and prune each other | §2.5, D2 |
| "move the records to the sidecar; that pass is fast, so the sidecar works" | Incomplete. The sidecar's own pass costs 1.19 s after an intervening `build`, for the same pruning reason. D1 alone would have inherited the defect it was fixing | §2.1, §2.3 |
| D1 with no discussion of invalidation | A durable memo is stale when the SubOS farm is re-pointed or `MCPP_ALLOW_HOST_LIBS` is toggled — neither is in the artifact's stat or the output fingerprint. The memoisation *creates* a correctness obligation that did not exist while the answer was recomputed | §2.5, D1a |
| "extend the fast path to `-p`" | Would promote an existing hole into the normal case. Measured: a new source file in a `path` dependency is invisible to the staleness sweep, and `mcpp build` reports success without compiling it. A defect in its own right, in no issue | §2.5, D3a; §0 |
| "a farm-origin provider that no dependency declares" | Wrong predicate. The farm is a symlink view *of installed packages*, so declared dependencies routinely resolve through it; #532's eleven-entry closure would have been reported wholesale. The predicate must canonicalise and ask who owns the file | §4.2, D8 |
| D13 as an error | Would turn green builds red with no defect behind them: a package declaring `standard = 26` compiles fine at 23 unless it uses a C++26 construct. Degraded first, `--strict` promotes | §5.5, D13 |
| D14 reading `[build] cxxflags` | Three other tables reach the compile line and not the BMI prebuild. Reading one table reproduces, inside the fix, the exact shape being fixed | §6.4 |
| "refuse `[toolchain] system`" | Measured: that configuration builds a project with `import std` today, in 2.04 s -- so an earlier draft proposed filling the variable and warning. §1.2 was then corrected per axis and the refusal reinstated: the measurement was right, the inference from it was not. See the third round below | §7, D15 |

**Second round, after the host-dependence policy was stated (§1.2).** The
policy — mcpp depends on no host; a user project may, and is warned rather than
refused whenever it builds and runs — is not a preference that could have been
guessed from the code, and four items moved once it was written down.

| claim as it stood | what the policy or a measurement showed | where |
|---|---|---|
| D15 refuses `toolchain = "system"` for `build.mcpp` | Reversed. A user project's opt-in to the host is warned about, not refused. `build.mcpp` failing is an unfilled variable (`tc->binaryPath` is already resolved), not a policy boundary, so the reporter's patch is adopted — on the narrower ground that it fills a variable, not that it adds host support | §7, D15 |
| "this document does not recommend the reporter's patch" | Withdrawn. It was excluded on a scope reading that §1.2 does not support | §9 |
| D13's declaredness bit suffices to keep the index quiet | False, and measurable: **782 of 782** index descriptors with an mcpp segment declare `language` (the key that feeds `Package::standard`), and **756 of 774** also say `import_std = false` — C libraries carrying a boilerplate `"c++23"`. A floor check keyed on declaredness would fire against the whole index for a root at c++20, which is exactly what §9-Q3 refused. D13 is now scoped to author-owned manifests | §5.5, D13a |
| D1 justified by "the sidecar is a cleaner owner" | The pattern already exists: `sync_resolution_verdict` (`runtime_validation.cppm:328`) already treats the sidecar as authoritative and `resolution.json` as a published copy. D1 is applying an existing convention, not choosing between two designs — and that also rules out the "make `prepare_build` merge" alternative, which would give one object two conventions | §2.5, D1 |
| D8/D9 as a build-time warning | Under-scoped. Host dependence is a distribution property: `mcpp.pack.host_requirements` exists to state what the target machine must supply, has two consumers, and warns in its own header that deriving the list twice is how they drift. D9 must feed it rather than stand up a parallel list | §4.2, D9; §12.6 |
| D8/D9 with no platform scope | ELF-only by construction — `runtime_search_closure` gives Mach-O and PE nothing. The record must say "not evaluated" on those platforms rather than be absent, or "not measured" and "clean" read the same | §12.1 |
| D14 as a complete check | GNU-spelled only. `/EHsc`, `/EHs-c-`, `/GR-` are the MSVC spellings of the same axes and are not in the list, while MSVC's std module already threads a CRT flag through for exactly this class of mismatch | §12.1 |

Three claims were checked and survived unchanged: `-fno-rtti` behaves
identically to `-fno-exceptions` (measured, same error); an auto-promoted
dialect flag in `cxxflags` builds silently (measured, green), now serving as
D14's control; and `mcpp pack`'s emitted manifest does not write `standard`, so
D10 does not leak into published descriptors.

**What would falsify the central diagnosis.** If, on the reporter's workspace,
the three stage timings do not account for the bulk of the 15.6 s, then §2 has
found a real defect that is not their defect, and the remaining time is
somewhere this document has not looked. The first thing to ask them for is the
`MCPP_VERBOSE=1` stage decomposition of one warm `mcpp test`; it is four lines
and it settles the question.

**Third round: the host-dependence rule is per AXIS, not one boundary.**

§1.2's first form applied a single test — "does it build and run" — to
everything, and D15 followed it to "warn, do not refuse". That was wrong, and
the error was not in the measurement:

| claim as it stood | what the corrected policy showed | where |
|---|---|---|
| one boundary for all host dependence | Two axes with different owners. The **toolchain** is mcpp's contract — `import std` availability, a computable closure, the same build elsewhere are all statements about a compiler mcpp resolved and can name — so it is refused. The **libraries** a program links are the program's own, and stay a warning | §1.2 |
| D15 fills the compiler path and warns | Reinstated as a refusal. Filling it makes an unsupported configuration *consistent*, which is not the same as making it supported; and a refusal that arrives as `posix_spawnp('')` three layers down is a bug wearing a policy | §7, D15 |
| `mcpp.diag::host_route_hint` shared by D8 and D15 | With the toolchain axis refusing rather than warning, and the library work not in this change, the helper had no consumer. Reverted — an unread field is the defect this document is about | §1.2 |
| three existing tests "still pass" | Two passed for the wrong reason. `14_toolchain_fallback`'s only assertion was a negative that any other error satisfies, and `293` began taking a skip its own header says must be earned. Verified by reading the branch each took, not the exit code | §7 |

The general lesson is the one this document keeps finding from the other side:
**a rule stated once, uniformly, is easier to write than one stated per axis —
and when the axes have different owners, the uniform version is wrong.** The
measurement that `system` builds and runs was correct and load-bearing for the
library axis; carrying it across to the toolchain axis is what produced a
proposal the maintainer had already declined on the issue.

**Fourth round: does the new key reach everything it says it reaches?**

Asked of the shipped implementation rather than of the design, and it did not.

| claim as shipped | what measurement showed | where |
|---|---|---|
| `[workspace.build]` applies to every member | Only to the member the command names. A sibling compiled as its `path` dependency — the ordinary workspace shape — got none of the flags, in the same command. `[workspace.package] standard` hid it, because the standard is imposed graph-wide from the root for BMI compatibility and reached the sibling anyway | §5.5 |
| a member may omit `version` when the workspace supplies it | True where the command names it; false where it is reached as a sibling's dependency, which refused it for a field the workspace does provide | §5.5 |
| `[workspace.build] include_dirs` inherits | It did, verbatim — so a relative path written at the workspace root was resolved against each MEMBER's directory. #224 for a third key, found by re-reading the merge | §5.5, D11 |

The shape all three share: **a rule was implemented at the one place its first
consumer reads it, and the key has more than one consumer.** The inheritance
site, the dependency load site and the package-assembly site each read a
different half, and a fix placed at any one of them is silent at the other two.
That is the same sentence as §2's defect, one layer up — which is why "who else
reads this?" is the question worth asking of every new key, and why the
denominator in each new test is a case that a single-site fix would still pass.
