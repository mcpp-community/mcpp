---
subject: design
status: active
---

# A pack's build reported as a build, and a unit's compile independent of the member selection: triage and design (#753, #751)

- Status: implemented as 2026.10.1.2 (pull request #754). Section 15 records
  what was built, what was measured, and where the implementation departs
  from sections 3, 4 and 10.
  - Revision 1 was reviewed on 2026-10-01. D1 to D4 were accepted as
    recommended: one placement rule for every plan, uniform PIC on ELF
    targets subject to M3, the module map as an argument file, and every
    group's build before one `Finished`. D5: one pull request.
  - Revision 2 adds the results of M1 and M2 (section 8), a review from
    several angles (section 12), the tasks and their dependencies (section
    13), and the work in other repositories (section 14).
- Date: 2026-10-01.
- Origin:
  1. mcpp#753: `mcpp pack` reports its build as `Planning` for as long as the
     build runs (release job of v2026.10.1.1: six minutes).
  2. mcpp#751: when two workspace members each compile a module of one name,
     a shared member's compile commands differ between `-p` and
     `--workspace`, so each switch recompiles it (GalTranslPP 3.1.3: 3m22s
     and 4m30s on Windows).
  3. The member selection plan of 2026.10.1.1
     (`2026-09-30-member-selection-and-build-program-cost-plan.md`, section
     8), which recorded #751 as outside that release and named its two
     possible remedies without choosing one.
- Task: separate what is mcpp's from what is not, and what is a defect from
  what is mcpp's design; then state one design for each defect at the level
  of its class, not of the reported instance.

## 0. Summary

| # | Finding | Source | Item |
|---|---|---|---|
| F1 | A cold `mcpp pack` prints no `Compiling` line and no `Finished`; the status row stays at `Planning` | measured, 2026.10.1.1 | A2, A3 |
| F2 | Every ninja drive assembles its own `BuildOptions`. Of ten construction sites, one sets the job count and one other attaches a report | code | A1, A2 |
| F3 | `[build] jobs = 3` reaches ninja as `-j3` from `mcpp build`, and not at all from `mcpp test` or `mcpp pack` | measured, 2026.10.1.1 | A1 |
| F4 | The reclaim of stale `detach-codegen` tokens runs only on the `mcpp build` path | code | A1 |
| F5 | A pack does not populate the global BMI cache for the dependencies it compiled | code | A4 |
| F6 | #751 reproduces: each switch between `--workspace` and `-p app` recompiles `core` | measured, 2026.10.1.1, llvm 22.1.8 | B1 |
| F7 | Two disjoint selections, `-p app` and `-p tool`, each with its own module `m`, place it at the same flat path and evict each other: each switch recompiles `core`. Not reported in #751 | measured | B1 |
| F8 | A workspace with a member that builds a shared library: `--workspace` adds `-fPIC` to every unit's flags, `-p app` does not, and each switch recompiles every member. No module is involved | measured | B2 |
| F9 | F6 to F8 share one cause: a fact computed over the plan's graph decides a unit's command line, and since 2026.9.29.1 a `-p` plan holds the selected closure while the build directory is shared by every selection of the configuration | code | B0 |

Two designs follow.

- **A. Every ninja drive of a plan is configured from the plan** (#753 and
  F2 to F5). The backend reads the job count and the token reclaim from the
  plan. A drive is reported unless its caller states that it reports the
  drive itself. `mcpp pack` writes `Finished` before `Packing`.
- **B. A unit's compile edge does not depend on the member selection**
  (#751 and F7, F8). The BMIs of every package other than the root are placed
  below that package's directory, as object files have been since #233. Each
  importer receives one module map derived from its own closure. Position
  independence becomes a property of the target and stops being a census of
  the graph.

## 1. Triage

### 1.1 #753

The whole report is mcpp's, and its expectation is mcpp's own stated
contract. `cmd_pack` opens the progress region with the comment "The build is
reported as `mcpp build` reports it; `Finished` closes the report before the
pack's own lines" (`src/cli/cmd_publish.cppm:204`). No build in the pack
pipeline attaches a report (`src/pack/pipeline.cppm:167`, `:1152`, `:1317`;
`src/pack/library_pipeline.cppm:189`). The comment states a contract that
the code does not implement.

The reported instance is one symptom of F2. The same omission drops the
declared job count (F3). A declared value that is silently dropped is a
defect, whatever feature it belongs to.

### 1.2 #751

The defect is real and general: it affects any workspace in which two
members provide one module name, which 2026.9.30.2 (W10, #732) made legal.
Four parts of the report are corrected or excluded.

| Claim in #751 | Verdict | Reason |
|---|---|---|
| "A member's compile commands depend on the member and its own dependencies, not on unrelated members of the graph" | **Too broad** | mcpp makes two inputs selection-dependent by design. Features are unified over the selection, Cargo-style (docs/06), and e2e 851 C asserts that `-p app23b` compiles `lib` without the feature only `app23` asks for. A member selected with `-p` holds the root's declaration privileges (e2e 850). The defect is a command line that changes while its meaning does not. Section 4.0 states the invariant in that form |
| "Then `-p` shares what `--workspace` built, as e2e 851 states" | **Incorrect citation** | 851 asserts one build directory per configuration and, in C, a recompile for a different feature union. It asserts nothing about recompiles |
| Remedy 2: "decide the per-provider mapping by what one program links" | **Insufficient alone** | With the placement still decided per graph, the `--workspace` graph would hold two BMIs at `pcm.cache/m.pcm`. A placement that does not depend on the graph is required, and that is remedy 1 |
| The comment's "CLI pack's tail" (8m40s at `Running 3/3`) | **Not designed for** | The comment states that it is not attributed. It was measured on 2026.9.30.2, whose per-member pack path 2026.10.1.1 replaced (#749). It is re-measured in the cross-verification (section 9) after B |

One part of the GalTranslPP case is the project's own: its updater and its
core each compile `3rdParty/3rdModule/boost.ixx`. W10 allows this and notes
that a package both depend on would compile the file once. Whether to move
the file into such a package is a usage choice, not an mcpp item.

## 2. Measurements

All runs use the released 2026.10.1.1
(`~/.xlings/data/xpkgs/xim-x-mcpp/2026.10.1.1/bin/mcpp`) on Linux x86_64 with
the machine's default toolchain, llvm 22.1.8. The fixtures are in the
session's scratchpad and are restated in the e2e criteria of section 10.

**F1.** A workspace `{util, app -> util, dso}`, `rm -rf target`, then
`mcpp pack -p app --format tar`. The output goes from `Target ...` directly
to `Packing app v0.1.0`; no `Compiling` and no `Finished` line appears. The
same tree under `mcpp build -p app` prints `Compiling util`,
`Compiling app` and `Finished`.

**F3.** A package with `[build] jobs = 3`, ninja argv traced with
`strace -f -e trace=execve`:

| command | ninja invocations | `-j` |
|---|---|---|
| `mcpp build` | 2 | `-j3`, `-j3` |
| `mcpp pack --format tar` | 2 | none (`--quiet`) |
| `mcpp test` | 3 | none |

Without `-j`, ninja runs `nproc + 2` compiles. docs/04 defines `jobs` as "how
many compiles run at once" and resolves `"auto"` against free memory,
because "a single module interface compile peaks at 0.5–1.0 GB". `test` and
`pack` exceed the bound that the key exists to enforce.

**F6.** The fixture of #751 (module renamed `.cppm`, `core`'s module named
`corelib`, since `core` is a reserved top-level name):

| step | compiled |
|---|---|
| `build --workspace` | core, tool, app |
| `build -p app` | core, app |
| `build -p app` | nothing |
| `build --workspace` | core, app |
| `build -p app` | core, app |

**F7.** The same workspace with `tool` given its own `m` (returns 7, `core`'s
returns 42): `-p app`, `-p tool`, `-p app` compiles `core` on the third step.
Every program prints its own value, so the results are correct; the cost is
the eviction. The build directory then holds `pcm.cache/m.pcm`,
`pcm.cache/core/m.pcm` and `pcm.cache/tool/m.pcm`.

**F8.** Workspace `{util (lib), app -> util (bin), dso (shared)}`, no module
name shared. Each switch between `--workspace` and `-p app` compiles `util`
and `app`. The `--workspace` build.ninja carries `-fPIC` in its global
`cxxflags`; the `-p app` build.ninja carries none. The flag comes from
`plan.needsPic` (`src/build/flags.cppm:733`), which `make_plan` sets when
any link unit of the graph is a shared library (`src/build/plan.cppm:3201`).
The same value enters the global BMI cache key (`src/build/cache_key.cppm:422`),
so a cached dependency is also served from two entries.

**Payload facts used below.** GCC 16.1.0 from xlings defines neither
`__PIC__` nor `__PIE__` by default. clang 22.1.8 for `x86_64-linux-gnu`
defines both as 2. xlings' build directory holds 144 BMIs.

## 3. Design A: every ninja drive of a plan is configured from the plan

### 3.1 The inventory

| site | report | `-j` from plan | token reclaim | cache populate |
|---|---|---|---|---|
| `mcpp build` / `run` (`execute.cppm:986`) | yes | yes | yes | yes |
| `mcpp test` phase A (`:2876`) | yes | no | no | yes |
| `mcpp test` bulk, per test, per member (`:2932`, `:3280`, `:3800`) | own lines, by design | no | no | — |
| `mcpp pack` group build (`pipeline.cppm:1317`) | no | no | no | no |
| `mcpp pack` dispatch pass (`:1152`) | no | no | no | — |
| `mcpp pack` Android legs (`:167`) | no | no | no | no |
| `mcpp pack` library (`library_pipeline.cppm:189`) | no | no | no | no |
| configure (`configure.cppm:92`, dry run) | — | — | — | — |

The comment above `scheduleNinjaJobs` (`execute.cppm:750`) states that
concurrency is resolved once and stamped on the plan "so this reads one
value instead of re-deriving it". One site reads that value.

### 3.2 Items

- **A1. The backend reads what the plan determines.**
  - The ninja argv takes `-j` from `plan.scheduleNinjaJobs` in
    `NinjaBackend::build`. `BuildOptions::parallelJobs` is removed: its only
    setter passes the same value.
  - The stale-token reclaim of a `detach-codegen` plan runs in the backend,
    once per build directory per command, before that directory's first
    drive. It cannot run before every drive, because a detached code
    generation may outlive the ninja that started it, and its token would
    then be live.
  - No caller can omit either setting, because none sets it.
- **A2. A drive is reported unless its caller reports it.**
  - `BuildOptions` gains `report`, with two values. `Region`, the default,
    means the backend attaches a `progress::Build` for the plan's directory
    whenever the command opened the region. `Caller` means the caller
    writes its own lines.
  - `mcpp test` passes `Caller` for its bulk, per-test and per-member
    drives, so their output does not change.
  - The default is the reported form. A new call site that says nothing is
    therefore reported, which is the opposite of today.
  - Machine output does not open the region, so `--message-format json` is
    unchanged.
- **A3. `mcpp pack` states its build as `mcpp build` does.**
  - It calls `programs_done()` before its first drive and
    `configurations(n)` for `n` configuration groups.
  - It writes one `Finished`, after the builds of all groups and before any
    staging. The pipeline therefore builds every group first, then stages
    and dispatches each group (D4). A group whose build fails is still
    reported alone (#749, P3).
  - The dispatch pass is a reported drive. Its steps are the format
    provider's actions and are attributed to that package. It writes no
    second `Finished`; `Distributing` and `Packed` follow it as today.
- **A4. One completion after a drive.** The population of the global BMI
  cache (`execute.cppm:1003`, `:2895`) moves into one function that `build`,
  `test` and every pack build call. `diag::flush` stays with each command,
  which owns its `--strict`.

### 3.3 What does not change

- `mcpp build`'s output, and `mcpp test`'s per-test lines.
- The JSON envelope of `mcpp pack --message-format json`.
- The build graph: no edge and no `build.ninja` byte changes.

## 4. Design B: a unit's compile edge does not depend on the selection

### 4.0 The invariant

A unit of package `P`, compiled in configuration `C`, has a compile edge
(command, outputs, inputs) that is a function of:

- `C`;
- `P` and `P`'s closure;
- the feature union the selection activates for `P` (docs/06, e2e 851 C);
- the declarations a selected member holds as the root (e2e 850).

No other fact about the plan's graph may reach a compile edge. The last two
inputs change what is compiled. Every other dependence on the selection
changes only how the same compile is spelled, and costs a recompile for
nothing. The invariant is stated in `docs/07-workspace.md` beside the
selection rules, and section 10 gives the test that enforces it.

The cause of F6 to F8 is a census: a value computed over the whole graph and
written into every unit. W10 states its condition over "the configuration"
(`2026-09-30-build-wall-time-progress-count-and-hang-plan.md`, W10:
"`<bmiDir>/<provider package>/<name><ext>` when it has more than one").
The implementation evaluates the condition over `graph.providersOf`
(`src/build/plan.cppm:3216`). Since 2026.9.29.1 (#738), a `-p` plan holds
only the selected closure, and the build directory is keyed by the
configuration and shared by every selection of it (e2e 851 C). A plan
therefore cannot observe the configuration that W10's condition names.

The codebase has met this machine before. Object paths were once decided by
a census over every unit. The comment at `src/build/plan.cppm:1755` records
three defects from it (#233, #240, #344) and its conclusion: "A conditional
layout is exactly the state that generated this bug family, and all it buys
is shorter paths." Dependencies' object paths have been unconditional since.
B1 applies the same rule to BMIs, and B2 to position independence.

### 4.1 B1: BMI placement by provider, and lookup by closure

- **Placement.** The root package's BMIs stay at `<bmiDir>/<name><ext>`.
  Every other package's BMIs go to `<bmiDir>/<package>/<name><ext>`,
  unconditionally. This is the object rule of #233.
  - A workspace plan's root is virtual and provides nothing. Every member is
    therefore qualified, in every selection.
  - `std` and `std.compat` belong to the configuration and stay flat.
  - The staging of a cached BMI uses the same function. The global cache's
    entries do not change, only the path to which an entry is staged.
- **Lookup.**
  - Each package whose closure provides a module outside the flat directory
    gets one module map. The map lists every named module its units can
    import (the closure's providers, `std`, `std.compat`, and names placed
    by other means). This is the map that W10 writes for GCC today, with
    the same content-hashed name (`modmap/<package>-<hash>.map`), and it is
    an input of every edge that reads it.
  - Each compile takes one argument:
    - GCC: `-fmodule-mapper=<map>`.
    - clang: `@<map>.rsp`, holding one `-fmodule-file=<name>=<path>` line per
      name.
    - MSVC: `@<map>.rsp`, holding `/reference <name>=<path>` lines.
  - Collation (`mcpp dyndep --module-map`) reads the same map, as it does
    for W10.
  - The resolver of W10 (`resolve_provider` over the importer's closure) is
    unchanged. Only the predicate that decides whether to use it changes.
- **What is removed.** The `collided` set, and the split between bound and
  unbound names: one path for every plan.
- **What changes, and for whom.**
  - Every project that imports a module from a dependency gets new BMI
    paths and one map argument per compile. It rebuilds once after the
    upgrade. The fingerprint does not contain the engine version, so the
    rebuild comes from ninja's command comparison, not from a new directory.
  - A project whose modules are all its own (and `std`) is byte-identical.
  - e2e 847 G changes from "no map without a collision" to "no map without
    a dependency module".
- **What it also removes.** F7: a qualified placement cannot be evicted by
  another selection. W10's clangd limitation, which concerns two providers
  of one name in one database, stays as documented.

### 4.2 B2: position independence is a property of the target

- **P1 (recommended).** Every unit is compiled with `-fPIC` on a target
  that has ELF shared objects: hosted Linux, the BSDs, and Android. Where
  the compiler already defaults to PIC, as on Android, the flag is
  redundant.
  - On PE/MSVC there is no flag, as today.
  - On Mach-O the compiler defaults to PIC, so nothing changes.
  - A freestanding target has no shared objects and gets no flag, as today.
  - `plan.needsPic` stops being a census. The `pic` field of the cache key
    becomes a function of the target, so a cached dependency is served from
    one entry whatever the selection.
  - This is rustc's default relocation model on these targets, chosen for
    the same reason: one artifact serves a program and a shared object.
- **P2.** PIC only for the units in the closure of a shared link unit of
  the plan. This is narrower, but still a census over consumers: a package
  that both a shared library and a program use differs between selections.
  It removes F8's instance and keeps its class.
- **The cost of P1 is measured before it is decided** (M3). On GCC 16, whose
  default is neither PIC nor PIE, P1 changes the code of every hosted ELF
  build. On clang 22 it changes `-fPIE` to `-fPIC`.

### 4.3 What B does not change

- The feature union and the root's declaration privileges: these stay
  selection-dependent by design (section 4.0).
- One build directory per configuration (#747, e2e 851).
- W10's rules: resolution in the importer's closure, refusal of two
  providers in one closure, and refusal of one file reached twice in one
  closure (e2e 847 A–F, 848).

## 5. Alternatives rejected

| Alternative | Why not |
|---|---|
| Take the census over the workspace's universe (every member's closure) at every plan | A `-p` plan would resolve and scan the closures of members it does not build. #738 plans `-p` on the selected closure because mcpp-index has 172 members and its CI runs `mcpp test -p` per member |
| Record the census in the build directory and read it back | Depends on order: a `-p` before the first `--workspace` still spells the commands differently |
| Put the census into the directory key | Each selection keeps its own warm directory. But `build --workspace` followed by `run -p X`, the measured case, then compiles in a cold directory |
| #751's remedy 2 alone | Two BMIs of one name at one flat path in the `--workspace` graph (section 1.2) |
| Per-package search paths (`-fprebuilt-module-path` and `/ifcSearchDir` for each closure package) | A stale BMI in one package's directory can answer for a name that another package of the closure now provides: a wrong answer instead of an error |
| Per-name flags on the command line | About 19 KB per clang compile for xlings' 144 BMIs. This approaches Windows' 32,767-character command line once paths are absolute |
| A: only attach a report at the four pack sites | Leaves F3 to F5. The next call site repeats the omission, because the default stays unreported |

## 6. Relation to W10

W10 rejected "explicit maps for every unit" because "it changes every
`build.ninja`, every database entry and every BMI path, with no gain for the
projects that have no collision". B1 is narrower: only projects with
dependency modules change. Its gain was not visible when W10 was written. A
collision is a property of the configuration, and no single plan can observe
the configuration since #738 and #747. This record therefore supersedes
W10's placement rule and keeps W10's resolution rule.

## 7. Decisions for review

- **D1. The scope of B1.**
  - Option (a), recommended: one rule for every plan, the #233 rule.
  - Option (b): the rule for workspace plans only. A single package's plan
    keeps W10's census, which is stable there because that package's graph
    is its configuration.
  - (b) changes no build of a project outside a workspace. (a) has one rule
    and no census, at the cost of one rebuild and new database entries for
    every project with dependency modules.
- **D2. Position independence.**
  - Option (a), recommended: P1, subject to M3.
  - Option (b): P2.
  - Option (c): take F8 out of this design into its own issue, and ship B1
    alone.
- **D3. The lookup spelling for clang and MSVC.**
  - Recommended: the map as a response file, one argument per compile. The
    compile database writes the flags expanded if M2 shows that a reader
    does not expand `@file`.
  - Alternative: per-name flags.
- **D4. The order of a pack over several configurations.**
  - Recommended: every group's build, then one `Finished`, then staging and
    dispatch.
  - Alternative: per group, with `Finished` deferred to the end, which then
    follows the first `Packing` line.
- **D5. Delivery.** A and B as one pull request and one release, or A
  first. A is small, and B changes every project with dependency modules.

## 8. Measurements before the implementation

| # | Question | Decides |
|---|---|---|
| M1 | On GCC 16, clang 22 and MSVC: a BMI compiled in one directory and staged from the cache into `<bmiDir>/<package>/` imports a second staged BMI. Does it load in a consumer whose map names both, when the paths recorded in the BMI differ from the paths of use? | B1 at all |
| M2 | Do clangd 22 and mcppls read a database entry that carries `@<map>.rsp` (clang) or `-fmodule-mapper=` (GCC)? | D3 |
| M3 | GCC 16 and clang 22, mcpp's own build and unit tests, with and without uniform `-fPIC`: object and binary size, build wall time, test run time | D2 |
| M4 | The length of the longest compile command of xlings on Windows, before and after B1 | D3 (confirmation) |

**Results (revision 2).**

- **M1, GCC 16.1.0 and clang 22.1.8: holds.** Module `A` imports `B`. Both
  were compiled in one directory, copied to two other directories
  (`pa/A`, `pb/B`), and the original directory was deleted. A consumer that
  imports both compiles, links and runs, given a GCC mapper file or a clang
  argument file that lists both. The consumer fails when the map lists only
  `A`: clang reports `failed to find module file for module 'B'` and GCC
  `B: error: failed to read compiled module`. The map must therefore hold the
  importer's whole closure, which section 4.1 already requires. MSVC is
  covered by e2e 848 on the Windows leg.
- **M2, clangd 22.1.8: holds.** clangd expands an `@file` argument relative
  to the database's `directory` and applies the `-fmodule-file=` lines it
  holds, with and without `--experimental-modules-support`, and reports no
  error.
- **M2, mcppls: holds.** mcppls expands response files relative to the
  entry's directory (`src/project/compdb.cpp`, `expand_response_files`),
  drops `-fmodule-file=` and `-fmodule-mapper=` as module mechanics
  (`src/spec/options.cpp`, S1-9-4), and drops an unexpanded
  `@<file>.modmap`, which is CMake's spelling of the same mechanism. The
  argument file is therefore named with the `.modmap` suffix. mcppls then
  reads it as module mechanics whether or not the file exists yet.
- M3 is measured on the implementation (section 13, T5). M4 reduces to one
  argument per compile under D3, and the Windows legs of CI confirm it.

## 9. Delivery and verification

- **mcpp.** One or two pull requests (D5), with the e2e criteria of section
  10 on the CI matrix (GCC and clang on Linux, clang on macOS, clang and
  MSVC on Windows).
- **Ecosystem.** mcppls is a release canary and reads the database (M2).
  xlings is built from the pull request branch: its build directory changes
  under B1 and must build, test and pack.
- **The validation project.** GalTranslPP is built from the mcpp pull
  request branch on a temporary branch, and both runs are green before the
  merge. Four checks:
  - `run -p GPPCLI` after `build --workspace` compiles nothing;
  - the pack shows `Compiling` and `Finished`;
  - the CLI pack's tail from #751's comment is re-measured;
  - the result is reported on #751.
- **mcpp's own release job.** Its `pack --target x86_64-linux-musl --mode
  static` shows the build's progress (#753's measurement).

## 10. Criteria

**A**

- (CHANGE) A cold `mcpp pack --format tar` prints a `Compiling` line for
  each package that compiles, and `Finished` before `Packing`. Under
  `--message-format json`, stdout is one JSON document. Fails on 2026.10.1.1.
- (CHANGE) `[build] jobs = 1`, with four translation units of about 0.3 s
  each. In `.ninja_log`, no two compile steps overlap under `build`, `test`
  or `pack`. Fails on 2026.10.1.1 for `test` and `pack`.
- (unit) With default `BuildOptions`, the ninja argv carries
  `-j<plan.scheduleNinjaJobs>`.
- (KEEP) `mcpp build`'s output, and `mcpp test`'s per-test lines.

**B.** One fixture workspace, run on GCC and clang (and on MSVC in Windows
CI), holds:

- a member reached by two members;
- two members each providing `m`: one from a distinct file, and one from
  the same file through `..`;
- a member with a shared target;
- a dependency from the index that provides a module and is served from the
  cache.

Its criteria:

- (CHANGE) For every member `M`, compare `mcpp emit build-database -p M`
  with `--workspace`. For every source of `M`'s closure, the entries' argument
  lists are equal. The entries are compared as parsed JSON, per source, not
  by substring. This fails on 2026.10.1.1 for F6 and F8.
- (CHANGE) `build --workspace`, then `build -p M` for every `M`: `.ninja_log`
  gains no compile entry. `-p app; -p tool; -p app`: the third gains none
  (F7).
- (KEEP) A variant with a feature difference: the two argument lists differ
  exactly by that feature's `-D`, which is e2e 851 C restated per entry.
- (KEEP) e2e 847 A–F, 848, 849, 850 and 851. (CHANGE) e2e 847 G, as in
  section 4.1.
- (KEEP) The byte comparison of `build.ninja` and `compile_commands.json`
  for every fixture whose modules are all root-owned.

## 11. Risks

- B1 moves the BMI paths of most real projects. Eleven e2e scripts read
  `pcm.cache/` or `gcm.cache/` paths, and each is reviewed for a flat-path
  assumption about a dependency's BMI.
- Under P1, every hosted ELF entry of the global cache is missed once,
  because its key changes.
- A third census may exist outside the inventory of section 4. The
  criterion of section 10 B is written to find one: it compares whole
  argument lists, not the two flags this design knows about.

## 12. Review from several angles (revision 2)

- **Architecture.**
  - A: every value a drive takes from the plan is read by the backend, the
    one component every command reaches. A caller states only what is its
    own: goals, keep-going, a timeout, and whether it writes its own lines.
  - B: one placement rule for objects and BMIs, decided by the provider
    alone. One resolver (W10's) answers every import. Position independence
    is read from the target, as the object format already is.
  - Removed: the `collided` census, `BuildOptions::parallelJobs`, the cache's
    exclusion of qualified BMIs, and the graph scan behind `needsPic`.
- **Stability.**
  - B1 moves the BMIs of every dependency. M1 establishes that a moved BMI
    loads through a complete map on GCC and clang. The cache-served case is
    an e2e on every leg, MSVC included.
  - The token reclaim runs once per build directory per command. A detached
    code generation of an earlier drive in the same command therefore keeps
    its token.
  - A pack builds every group before staging any of them. A group's failure
    stays its own, as #749 requires.
- **Simplicity.** B1 deletes a branch rather than adding one: a plan without
  dependency modules and a plan with a collision take the same path. A adds
  one enumerator to `BuildOptions` and removes one field.
- **User experience.**
  - `mcpp pack` shows the build it performs: the package lines, the status
    row and `Finished`.
  - `[build] jobs`, `--jobs` and `MCPP_JOBS` bound every command that
    compiles.
  - Switching between `-p` and `--workspace` recompiles only what the
    feature union changes.
- **Compatibility.**
  - No manifest key, no command-line option and no JSON field is added or
    changed.
  - The global cache keeps its entry layout. Under P1 the `pic` field of
    hosted ELF keys changes, so those entries are missed once and then
    refilled.
  - The build database gains one argument per compile for a package that
    imports a dependency's module: `-fmodule-mapper=` on GCC, `@<map>.modmap`
    elsewhere. Both readers of the ecosystem accept it (M2).
- **Platforms.**
  - GCC reads the mapper file. clang reads the argument file, which is
    tokenised by the host's rules, so a path is quoted when it holds a space
    or a quote. MSVC reads `/reference` lines from a UTF-8 file with a byte
    order mark, as every response file mcpp writes for an MSVC tool.
  - P1 applies to ELF only. Mach-O already defaults to PIC, PE has no such
    flag, and WebAssembly and freestanding targets have no shared objects of
    the kind the flag serves.
- **Consistency.** `build`, `test` and `pack` drive ninja through one
  backend with the same job count and the same report. The rule of #233 now
  covers both kinds of build output that a package contributes.
- **Upgrade without action.** Nothing is to be done by a user. The first
  build after the upgrade recompiles a project that imports dependency
  modules (its command lines changed) and, on hosted ELF, every project
  (`-fPIC`). The build directory, whose key does not change, is reused.
- **Test coverage.** Every item has a criterion that fails on 2026.10.1.1
  (section 10). The property test of section 10 B compares whole argument
  lists, so it also detects a census that this design has not listed.

## 13. Tasks and dependencies

| Task | Content | Depends on |
|---|---|---|
| T1 | A1: `-j` and the token reclaim in the backend; `parallelJobs` removed | — |
| T2 | A2: `BuildOptions::report`; the backend attaches the report; `mcpp test`'s own drives pass `Caller` | T1 (same function) |
| T3 | A3: `mcpp pack` and the library pack state their build; builds before staging; the dispatch pass is reported | T2 |
| T4 | A4: one function populates the global cache after a drive; called by `build`, `test` and `pack` | — |
| T5 | B2: position independence from the target; M3 measured on mcpp's own build | — |
| T6 | B1: placement by provider; the map for every importer of a qualified module; the `.modmap` argument file; the cache's BMI artifacts carry their build path | — |
| T7 | Unit tests and e2e for T1 to T6 (section 10); existing e2e that read flat BMI paths reviewed | T1–T6 |
| T8 | Documentation (docs/04, 05, 07, 10, 91, the build-database specification, their Chinese counterparts), CHANGELOG, version 2026.10.1.2 | T1–T6 |
| T9 | Pull request; CI on every leg; GalTranslPP built from the branch | T7, T8 |
| T10 | Review of the pull request and of the ecosystem | T9 |
| T11 | Merge, release, GitCode assets, xim-pkgindex, mcpp-index | T10 |
| T12 | Sandbox verification from the index; comments on and closure of #751 and #753 | T11 |

T1, T4, T5 and T6 touch disjoint code and may be done in any order. They are
done in one branch, by one author, because T6 and T5 both change the
command lines that T7's criteria compare.

## 14. Work in other repositories

| Repository | Work | When |
|---|---|---|
| Sunrisepeak/GalTranslPP, PR 3 | A temporary branch builds mcpp from the pull request and runs the workflow; both runs green is a merge condition. After the release, PR 3 runs on the released version | T9, T12 |
| mcpp-community/mcpp-language-server | Release canary; no change expected (M2) | T11 |
| openxlings/xlings | Release canary; built and tested with the release in the sandbox; no change expected | T11, T12 |
| openxlings/xim-pkgindex | The release workflow opens the version bump; merged by a maintainer | T11 |
| mcpp-community/mcpp-index | The `latest_mcpp` pin moves to the release, and the full sweep runs | T11 |

## 15. Implementation record

### 15.1 Departures from sections 3, 4 and 10

- **The module map is per unit, not per package (B1).** `resolve_provider`
  answers a name's single provider whether or not it lies in the importer's
  closure, so a map derived from it lists other members' modules under
  `--workspace` and not under `-p`. A package's closure also differs between
  `mcpp build` and `mcpp test`, which adds the dev-dependencies. The map
  therefore lists what one unit reaches through its imports (and, for GCC,
  the module it provides and `std`), resolved hop by hop in each importer's
  closure. Units with equal maps share one file.
- **A third census (B1).** The whole-argument-list criterion found that a
  file a member lists from outside its directory (`../shared/m.cppm`, the
  GalTranslPP shape) was owned by the workspace's virtual root, whose
  directory is the workspace's. It entered the root's basename census, so its
  object was `obj/m.m.o` under `-p` and `obj/core/__pkg/workspace/shared/m.m.o`
  under `--workspace`. A workspace plan's root now owns no source; such a unit
  belongs to the member that declares it.
- **The map's key.** A module and a unit that imports it can hold the same map
  and differ in what they load (only the importer has an argument file). The
  key hashes the map and the relative paths the argument file loads, so it is
  the same in every build directory, as the map is.
- **`emit build-database` writes the maps** into its work directory, so a
  reader that expands the argument files finds them.
- **The cache artifact.** `DepArtifacts` names each BMI's build path in a
  vector of pairs (`BmiPlacement`). A `std::map<std::string, std::string,
  std::less<>>` in that struct made clang 22.1.8 crash (SIGSEGV in
  `ASTReader::readTypeRecord`) compiling every importer of
  `mcpp.build.prepare` that instantiates a ranges algorithm, on Linux and
  macOS; GCC 16 compiled it. The first CI round of #754 failed on every macOS
  leg for this reason, and the cause was bisected locally.
- **The MSVC argument file** holds each `/reference` and its value on one
  line. cl.exe does not take an option's value from the next line of a
  command file: the second CI round of #754 failed e2e 258, 262 and 848 on
  the Windows legs with `D8004: '/reference' requires an argument`. A unit
  test now writes both compilers' files and reads them back.
- **The review of the pull request** (section 15.3) found five more places,
  each fixed with a unit test:
  - `mcpp run --format <name>` wrote `Finished` twice, once for the pack and
    once for the build it drives again to resolve the runner. `Finished` is
    now written once per command, in the progress model.
  - An eleventh drive site, the sub-build of a dependency's host tool inside
    planning (`prepare/features.cpp`), took the new default and was reported
    as lines of the command, while its caller folds its output into an error
    message. It states `Caller`.
  - An argument file holds the build directory's absolute path while its name
    hashes relative paths, so a moved or restored build directory kept the
    old file. Both module map files are now written whenever their content
    differs.
  - The root was recognised by the name `workspace`; a member of that name
    would have kept its BMIs at their names. A virtual root is recognised by
    `virtualRoot`.
  - A member's file inside another member's directory was addressed under
    the containing package, which the virtual root replaced when the selection
    did not hold that member. A workspace plan addresses such a file by its
    place in the workspace (`obj/<member>/__ws/<path>`).
- **B2's predicate** is "ELF and not freestanding", read from the target
  triple with the host triple when the target is empty. Mach-O is excluded
  because its compilers default to PIC; WebAssembly refuses shared objects.
- **A2.** `mcpp test`'s bulk, per-test and per-member drives pass `Caller`.
  The dispatch pass of a pack reopens the report, because `Finished` closes
  it. The library pack and the Android legs are reported by the default.
- **e2e numbers.** 860 and 861 were already taken on main, so the criteria of
  section 10 are e2e 871 (A) and 872 (B). e2e 847 G holds unchanged (the
  updater alone is a root package); 847 A, 09 and 849 B are restated for the
  new placement.

### 15.2 Measurements

- **M3, GCC 16.1.0, mcpp's own release build (`--cache off`), the same
  sources built by 2026.10.1.1 (no PIC) and by this branch (PIC):** wall time
  111.52 s and 111.95 s; binary 25,184,592 and 25,227,320 bytes (+0.17%), text
  11,669,387 and 11,684,419 bytes; a full plan of the mcpp tree (`emit
  build-database`, median of ten alternating runs) 0.167 s and 0.169 s.
- **The reproductions of section 2 on this branch:** F6 and F8 compile nothing
  after the first `--workspace`; F7's `--workspace` after `-p app` and
  `-p tool` archives `bin/core/libcore.a`, the one product neither `-p` build
  needs, and compiles nothing.
- **Self-hosting:** mcpp builds itself with this branch under GCC 16 (204
  module maps; a repeat build is a no-op; an edit recompiles one unit) and
  under clang 22 (`--dev`).
- **Local suites (clang 22 default host):** unit tests 143 of 143; e2e 477
  passed, 21 failed, 61 skipped, and each of the 21 fails in the same way on
  2026.10.1.1 except 872, whose run used a binary older than its script, and
  178, which passed on both when repeated. e2e 871 and 872 pass under GCC 16
  and clang 22 and fail on 2026.10.1.1.
- **The sandbox script** `.agents/docs/2026-10-01-pack-drive-and-selection-verify.sh`,
  on the host against this branch: 9 of 9 sections pass, xlings built from its
  source among them; against 2026.10.1.1 every CHANGE section fails.

### 15.3 Review

A review of the full diff, independent of the author, read the plan, backend,
pack, cache and progress code against this record. Its three findings above
medium confidence and its two below were confirmed in the code and fixed
(section 15.1). It found the per-unit maps, their keys, the dyndep and staging
paths, the cache population, both argument-file writers, the job count and the
token reclaim consistent with sections 3 and 4.

