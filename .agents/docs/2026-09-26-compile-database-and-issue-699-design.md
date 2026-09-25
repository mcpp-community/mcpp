---
subject: design
status: active
---

# The compile database, `emit build-database`, and #701/#702: triage against the specifications, and one design

- Sources:
  - The report *compile_commands.json 对比分析：xmake vs mcpp*
    (`/home/speak/test/mcpp/hello/cdb-report/README.md`, 2026-09-26), measured with mcpp
    2026.9.21.3 and clangd 22.1.8. Cited as "the report". Its xmake comparison is a reference
    only; every verdict below is taken against a specification.
  - mcpp-community/mcpp#699 (2026-09-25), *emit build-database for IDEs: one failing member
    loses the whole database, and build programs cannot tell a plan pass*, from
    Sunrisepeak/mcpp-language-server#23 and #24 on the GalTranslPP workspace.
  - mcpp-community/mcpp#701 and pull request #702 (2026-09-25, head `c57c12f2`): a build
    program declares a runtime library directory, and a passing check moves its stamp. Their
    consumer is mcpp-plugins 0.13.0 (`deps-vcpkg`, `deps-cmake`, `rules-qt`), whose design
    record (branch `feat/deps-vcpkg-rules-qt`, `.agents/docs/2026-09-26-deps-vcpkg-rules-qt-design.md`,
    decision D8) defers #702 and builds on the released 2026.9.26.1 instead.
  - Earlier records of C1: #397 item C-1 (2026-08-09, P0) and #677 item B1 (2026-09-20).
- Specifications: the JSON Compilation Database
  (<https://clang.llvm.org/docs/JSONCompilationDatabase.html>); S1 *C++ Build Database: IDE
  Profile* 0.2.0 and S2 *discovery*, both in `Sunrisepeak/mcpp-language-server/docs/specs/`,
  which SPEC-005 implements; SPEC-005; docs/50; docs/04 §2.11; docs/30.
- Consumers: **mcpp-language-server (mcppls) is the primary one.** It reads the S1 document
  that `emit build-database` prints and drives clangd through a database it writes itself
  (its `docs/90-architecture.md`). `compile_commands.json` serves the tools that read the JSON
  format directly: clangd without mcppls, clang-tidy, and others.
- Basis: `origin/main` 3a6ac7f4 (mcpp 2026.9.26.1 and one records-only commit); #702 at
  `c57c12f2`. Code citations are to those commits.
- Measurements: Linux x86_64 (Ubuntu 24.04); the released mcpp 2026.9.26.1 invoked by its xlings
  store path; toolchains llvm@22.1.8, llvm@20.1.7, gcc@16.1.0; clangd and clang-tidy 22.1.8; CMake
  4.4.2. Every project is a copy or a fixture in a scratch directory. Appendix A holds the
  readings.
- Status: accepted; implemented in #702 (§11).
  - Revision 1 (2026-09-26): the triage and the first design.
  - Revision 2 (2026-09-26): D1 and D3 accepted; mcppls named as the primary consumer; C3
    measured against the specification and three build systems; C5 rewritten around its four
    questions; C1 and C2 replaced by one database per configuration (D6); #701 and #702
    reviewed (§5), with the defect F1 they led to; an overall review (§10).
  - Revision 3 (2026-09-26): D2, D4, D5, D6, D7 and D8 accepted; F1 filed as #703; the
    conformant design of #701/#702 (§5.4: `runtime_search_dir`, the stamp rule, the `prepare`
    role, and W, the placement of a Windows program's DLLs); SPEC-007, the build plugin specification (`docs/specs/build-plugins.md`, §5.6); #702
    reused as the single pull request (§7).
  - Revision 4 (2026-09-26): D9 to D12 accepted; the implementation plan, its task
    dependencies and the cross-repository sequence (§11).

---

## 0. Summary

| | Finding | Source | Verdict | Repair |
|---|---|---|---|---|
| **C1** | A deleted `compile_commands.json` is not written again by the next `mcpp build` | report §3.6; #397 C-1; #677 B1 | **mcpp defect** | §3.2: the fast path restores the root database from the configuration's database |
| **C2** | The merge resolves `file` against the process's working directory, keeps other tools' entries, and mixes two toolchains in one file | report §3.1; the mixing is new | **mcpp defect** | §3.2: one database per configuration; the root file is replaced, never merged (D6) |
| **C3** | `directory` (and S1 `work-directory`) name the project root; the compiler runs in the output directory | report §3.2 | **mcpp defect.** The JSON format and S1-8-2 define the field as the directory the compiler runs in; CMake, ninja and xmake write that directory | §3.3 (D2) |
| **C4** | A module interface's language flag is missing from both databases | new | **mcpp defect**: clangd cannot handle a `.ixx` entry | §3.4 |
| **C5** | `compile_commands.json` lists no standard-library unit | report §3.3 | **mcpp gap**, contrary to S1-12-1 | §3.5 (D5) |
| **C6** | The database is written at the project root | report §3.5 | by design; the symlink redirect is undocumented | documentation |
| **E1** | One member's planning failure removes every member's sets | #699 item 1 | **mcpp gap** | §4.4 (D1, accepted) |
| **E2** | Under `emit`, a host tool whose build fails ends the requesting member's plan | #699 item 2 | **mcpp gap** | §4.5 |
| **E3** | A failing build program ends its member's plan | #699 item 2 | **mcpp gap** | §4.6 (D3, accepted) |
| **E4** | A signal that tells a build program it runs for a plan | #699 item 2 | **not adopted** | §4.7 (D4) |
| **R1** | #701/#702: `mcpp::runtime_library_dir(dir)` | #701 | the gap is real; as written it extends a field docs/04 retires | §5.4 R1': `runtime_search_dir` on `LinkIntent` (D7, accepted) |
| **R2** | #701/#702: a passing check moves its stamp | #701 | **mcpp defect**; the fix is correct | §5.4 R2: every stamp newer than every input (D10) |
| **F1** | `$ORIGIN` in a user link flag reaches the program as `/../lib` | the plugin record; measured here | **mcpp defect**, filed as #703 | §5.5: SPEC-004 §8 extends to link flags (D8, accepted) |
| **O1** | deps-cmake and deps-vcpkg run installation as `check` actions | the plugin record | design gap: no action role describes construction whose file names are unknown | §5.4 P: the `prepare` role (D9) |
| **O2** | on Windows, a program started by hand does not find DLLs from a runtime search directory | this review | framework gap | §5.4 W: placement after the link (D11) |
| **S7** | a contract for build plugins | this review | new specification | SPEC-007, draft 0.1 (§5.6, D12) |
| | xmake's database; GalTranslPP's incomplete Qt and its environment checks inside build programs | report §3.4; #699 | not mcpp | §3.7, §4.8 |

Three statements:

1. **A database states the build's facts, one configuration at a time.** `directory` is where
   the compiler runs (C3), `arguments` carry the flags that decide how it reads its input (C4),
   and one file holds one configuration (C1, C2). Where mcpp departs from this, it departs from
   the JSON format, from S1 and from every build system measured.
2. **mcpp compiles nothing twice.** A second compile of `std` is a reader's, and it happens
   because a BMI is readable only by the compiler that wrote it. The database's task is to name
   both the provider, so any reader can compile, and the build's BMI, so a matching reader can
   reuse it (C5).
3. **#699, #701 and #702 are one family:** a build program configures, actions construct and
   verify, and a new engine surface joins the current model rather than a retiring one.

## 1. Method

Each claim was reproduced on the current release in a copy or a fixture, located in the code,
and classified against the contract that governs it. A repair is placed where the rule is
stated, with a criterion that fails on 2026.9.26.1.

| Verdict | Meaning |
|---|---|
| mcpp defect | mcpp violates a contract it states or a specification it claims to follow |
| mcpp gap | mcpp does what its specification says; the specification omits a case its consumer needs |
| by design | deliberate, and the reasons still hold |
| usage / not mcpp | the effect belongs to a project, an environment or another program |

## 2. Principles

- **P1. One record, two documents.** `compile_commands.json`, `emit --spec compile-commands` and
  the S1 document render one `UnitInvocation` per unit (`src/build/compile_commands.cppm:49-64`;
  SPEC-005 R3.7). C3 and C4 change that record; no output is repaired on its own.
- **P2. A field states what the build does.** The JSON format defines `directory` as "the
  working directory of the compilation" and `arguments` as the argument vector that "should run
  the compilation step"; S1-8-2 defines `work-directory` as the "absolute path of the directory
  the compiler runs in".
- **P3. The fast path replays a build.** Every product of the full path is produced by the fast
  path or verified by it.
- **P4. A description names what it could not describe, per unit**: a member for a planning
  failure, a package's directives for a build-program failure, nothing for a construction
  failure.
- **P5. Configuration is not verification.** A build program configures; a `check` action
  verifies. `mcpp build` runs both; `emit` runs the first.
- **P6. One program behaviour.** The plan `emit` describes is the plan
  `mcpp build --configure-only` computes (R1.2).
- **P7. One database, one configuration.** A configuration is what the fingerprint names:
  toolchain, target, profile, features. A file that mixes two configurations describes no build.
- **P8. A new surface joins the current model.** docs/04 §2.11 keeps `[runtime] library_dirs`
  "for one compatibility train" and names its successor, `runtime_search_dirs`.
- **P9. The engine provides general mechanisms; a tool's knowledge stays in its plugin.** A gap a
  plugin meets is closed in the engine only when it is general, and then by a mechanism that names
  no tool (R1', R2, P and W below); vcpkg, CMake and Qt are never named by the engine.

## 3. The compile database

### 3.1 Readings on 2026.9.26.1

| Claim | Reading on 2026.9.26.1 | Finding |
|---|---|---|
| deleted database not rewritten (report: "not reproduced") | `mcpp build; rm compile_commands.json; mcpp build` prints `Finished dev in 0.00s` and leaves no file, on every attempt; `--no-cache` writes it | C1 |
| relative entries of another writer | kept from the project root (6 entries: `src/main.cpp` beside `/…/src/main.cpp`), pruned from `src/` (4 entries) | C2 |
| (new) two toolchains | after `mcpp test` (llvm) and `mcpp build --toolchain gcc@16.1.0`: three `g++` entries and one `clang++` entry | C2 |
| GCC entries cannot be replayed from `directory` | from `directory`: `test.cppm` and `main.cpp` fail and `gcm.cache/` appears in the project root; from the output directory: 3 of 3 write their objects; clang: 3 of 3 from either | C3 |
| omitted `-x c++-module` is harmless | a `.ixx` interface declared in `module_extensions`: clangd `[fe_expected_compiler_job]`; with the flag, 0 errors; GCC 16 compiles `.ixx` without it | C4 |
| no std unit | §3.5 | C5 |

### 3.2 One database per configuration (C1, C2; D6)

**What is wrong today.** `publish_compile_commands` merges the fresh plan into whatever the root
file holds (`src/build/compile_commands.cppm:272-319`): a prior entry survives when its `file`,
compared as written and probed against the process's working directory (`:288-311`), is absent
from the fresh plan and exists. The rule exists to keep the units of an earlier `mcpp test`
(`:69-79`), and it keeps every other entry too: another tool's, and mcpp's own from a previous
toolchain. Separately, the fast path never reaches the writer (`src/build/execute.cppm:1408-1530`
against `src/build/ninja_backend.cppm:3474`), so a deleted file stays deleted (C1); `git clean -fd`
reaches the same state, because the `.gitignore` that `mcpp new` writes lists `target/` and
`.mcpp/` only (`src/scaffold/create.cppm:407`).

**The identity of a configuration.** mcpp already names every configuration: the output
directory `target/<triple>/<fingerprint>`, derived from the toolchain, target, profile and
features. `mcpp build` and `mcpp test` in one configuration share it (measured: after both, the
build's entries and the test entry name one fingerprint directory; the fingerprint deliberately
covers neither tests nor dev-dependencies, #407). After C3, it is the `directory` of every project
entry. A field of mcpp's own inside an entry is not an option: one unknown key makes clangd
report `Failed to load compilation database` and clang-tidy refuse the file (measured). S1
consumers ignore unknown fields (S1-11.2-1); JSON-format readers do not.

**Design.**

1. **The configuration's database** is `target/<triple>/<fingerprint>/compile_commands.json`.
   Every command that plans in that configuration (`build`, `test`, `run`, `--configure-only`)
   writes it: the fresh plan's entries, plus the entries it already holds whose `file`,
   resolved against their `directory`, the fresh plan lacks and which still exist. Every entry
   in it was written by mcpp in this configuration, so no ownership test is needed and none is
   made.
2. **The root file is a copy of the current configuration's database**, replaced whole and never
   merged, and left untouched when identical, so that clangd is not triggered for nothing.
   Switching toolchain or profile therefore switches the whole file, and switching back restores
   that configuration's entries, its test units included. A symlink at the root is written
   through, as today.
3. **Another writer's entries are replaced, and said so.** When the replaced root file held
   entries mcpp did not write, one warning states their number and that the file holds mcpp's
   configuration. An entry is mcpp's when its `output` lies under this project's `target/` or
   under the mcpp home, where the standard-library units of C5 write their objects; `directory`
   cannot decide it, because those units run in the std cache.
4. **The fast path restores the root file (C1).** It knows the configuration's directory
   (`match->outputDir`) and publishes the root file through the same function as the full path:
   when the root file is missing or differs from the configuration's database, it is replaced,
   with the same warning. No plan is needed, and P3 holds.
5. `emit build-database` writes neither file (R2.1), and prints its plan as today.
6. The scaffold's `.gitignore` also lists `compile_commands.json`: it names absolute paths of one
   machine.

This replaces the four-condition merge of revision 1. The answer to D6 is **replace across
configurations, merge only within one**, with the output directory as the identifier: a value
mcpp already computes, visible in a standard field, and stable across the commands that should
share a database.

**Criteria.** (1) e2e: plain build; delete the root file; `mcpp build` restores it without a
plan (fails on 2026.9.26.1). (2) e2e: the root file replaced by another writer's entries;
`mcpp build` leaves only the configuration's entries and warns once. (3) e2e: `mcpp test`, then
`mcpp build`, same configuration: the test entries remain. (4) e2e: llvm, then gcc, then llvm:
the root holds one toolchain's entries each time, and the llvm test entries return with llvm.
(5) unit: the within-configuration merge resolves `file` against `directory`.

### 3.3 C3: `directory` (D2)

**The specifications.** The JSON format: "directory: The working directory of the compilation.
All paths specified in the command or file fields must be either absolute or relative to this
directory." S1-8-2: `work-directory` is the "absolute path of the directory the compiler runs
in", and S1-12-1 exports it as `directory`.

**What mcpp does.** `unit_invocations` writes `plan.projectRoot` (`compile_commands.cppm:231`);
ninja runs every compile in the output directory (`ninja -C <outputDir>`: `execute.cppm:1193`,
`ninja_backend.cppm:3653`). The standard-library units already state their real directory,
recovered from the command mcpp runs (`src/build/build_database.cppm:315-372`).

**What other producers do** (measured; the same two-directory project for CMake):

| Producer | `directory` | Where the compiler runs | Replay from `directory` |
|---|---|---|---|
| CMake 4.4.2, Ninja generator | the build directory | the build directory | object written |
| CMake 4.4.2, Unix Makefiles | the target's build subdirectory (`<build>/lib`) | that subdirectory | object written |
| `ninja -t compdb` over mcpp's own `build.ninja` | `target/<triple>/<fingerprint>` | the same | |
| xmake (the report's sample) | the project root | the project root: its `file` and `-o` are relative to it, and the report's replay passed 3 of 3 | objects written |
| mcpp 2026.9.26.1 | the project root | `target/<triple>/<fingerprint>` | GCC importers fail; `gcm.cache/` written into the source tree |

Every producer names the directory its compiler runs in, whatever that directory is; mcpp is the
one that does not, and ninja reading mcpp's own graph names the directory mcpp should.

**Repair.** `directory` and `work-directory` are the output directory, for every unit and every
toolchain. The directory exists whenever the database does: the full path writes `build.ninja`
into it, and `emit` plans into a work directory under the mcpp home (measured: it exists after
`emit`).

**Consequence.** A tool that changes into `directory` needs it to exist: clang-tidy 22.1.8 aborts
with `LLVM ERROR: Cannot chdir into "…"!` when it does not, while clangd logs
`VFS: failed to set CWD` and continues (both measured). After `mcpp clean` the root file names a
directory that is gone until the next build; the module arguments of the same entries
(`-fmodule-file=`, `-fprebuilt-module-path=`) already point into it, so a cleaned tree already
fails for every module unit. With §3.2, the next build replaces the root file anyway.

**Criteria.** (1) e2e, gcc and llvm rows: every entry replayed from its `directory` writes its
object, and no `gcm.cache/` appears under the project root (fails for gcc on 2026.9.26.1).
(2) unit: a project unit's `directory` is `plan.outputDir`. e2e 211 derives a sibling fixture
from `directory` (`tests/e2e/211_configure_only_cdb.sh:77-78`) and changes with it.

### 3.4 C4: the interface language flag

The build states a module interface's language explicitly (`BmiTraits::moduleInterfaceLangFlag`:
`-x c++-module` for clang, `-x c++` for GCC, `/interface /TP` for MSVC;
`modules/toolchain-model/src/model.cppm:456-683`), so that no driver infers it from an extension
(`modules/source-kind/src/source_kind.cppm:360-371`: "mcpp never lets it guess").
`unit_invocations` omits it (`compile_commands.cppm:234-246`), and a reader infers after all:
clang does not know `.ixx` and hands the file to the linker, so clangd has no compiler job for it
(measured); GCC 16 compiles `.ixx` and `.cppm` without the flag.

**Repair.** The record carries the flag at the position the build uses, before `-c <source>`,
for every unit whose kind produces a BMI; in S1 it lands in the interface units'
`local-arguments`. `-MMD -MF` and `-fmodule-output=` stay omitted: they name side outputs and do
not change how the input is read.

**Criteria.** (1) unit: an interface unit's arguments carry the dialect's flag before `-c`; an
implementation unit's do not. (2) e2e, llvm row, `module_extensions = [".ixx"]`: the interface
entry, replayed from its `directory`, writes an object. **Open:** whether clangd in clang-cl
mode accepts `/interface` is measured on Windows CI before the MSVC form is emitted.

### 3.5 C5: the standard-library units (D5)

**Does mcpp compile `std` twice?** No. mcpp builds the standard-library modules once per
toolchain, standard and flags into the shared std cache under the mcpp home, which every project
reuses; `emit` describes them and compiles nothing (R2.2). Listing a unit in a database compiles
nothing on mcpp's side.

**Was leaving them out of `compile_commands.json` correct?** It was a choice (SPEC-005 R4.1),
and the wrong one.

- The JSON format describes "one way a translation unit is compiled in the project". The build
  compiles `std.cppm` and links its object into the program, so an entry for it states a fact of
  the build. The format has no completeness rule, so the omission breaks no sentence of it.
- S1-12-1, the export rule of the profile mcpp implements, makes **every** translation unit of
  the S1 document an entry, and mcpp's S1 document contains the std units (R3.10). R4.1
  contradicts it.
- The omission is what binds a reader to the toolchain's exact version: with no provider in the
  database, clangd falls back to the prebuilt BMI the importers name, and fails on another
  version (measured below).

**Why does a reader compile `std` again, then?** Because a BMI is readable only by the compiler
that wrote it: clangd 22.1.8 on a BMI written by clang 20.1.7 reports `ast_file_version_too_old`,
and S1 §6 adds that "equal versions do not imply compatible BMIs" (`build-id` decides). S1-11.2-3
therefore forbids a consumer to use build BMIs unless its engine matches the toolchain's
`family`, `version` and `build-id` exactly, the user has enabled an authoritative mode, and the
consumer can detect staleness. clangd's own policy (measured) is to build a module whose provider
the database lists, and to load a prebuilt file only when none is listed. The second compile is
the reader's own BMI, made for the reader's own compiler.

**Measurement** (an interface importing `std`, an importer; clangd 22.1.8 `--check
--experimental-modules-support`):

| Database | clangd errors |
|---|---|
| llvm@20.1.7, as written | 1: `Failed to build module std; due to Don't get the module unit for module std`, then `[ast_file_version_too_old]` |
| the same, with the two `mcpp:std` units appended | 0: `Built module std`, `Built module hello.greet` |
| gcc@16.1.0, as written | 1: `module 'std' not found` |
| the same, with GCC's `bits/std.cc` unit appended | 1: clang's scan of `bits/std.cc` under GCC's command fails |

**The better answer: name both, and let the reader choose.**

- **D5a.** `compile_commands.json` lists the standard-library units whenever the build imports
  `std`, with the fields S1 gives them (R3.10, R3.11), and R4.1 follows S1-12-1. A reader of another
  clang version can then build `std` (measured: clangd 22.1.8 on an llvm@20.1.7 database); the importers keep naming the build's BMI (P2), which a
  reader of the same version may load. The cost falls on a matching plain clangd, which builds
  `std` once per session instead of loading it (about 1.4 s in the measurement).
- **D5b.** The S1 document names the build's BMI for the standard-library units: `provides`
  maps `std` and `std.compat` to the BMI paths in the shared std cache (S1-8-6: "the path of the
  BMI the build writes"), which `emit` and the build share, and `ide.toolchains.<id>.build-id`
  carries the compiler's build identity (S1 §6, MAY). An mcppls whose engine matches exactly,
  in the authoritative mode S1-11.2-3 describes, then reuses mcpp's BMI and compiles nothing;
  any other reader compiles, as S1 requires. Project modules keep `""` under `emit` (S1-8-6
  allows it), because the planning pass writes into its own work directory.
- For mcppls, D5a changes nothing: it exports S1 itself and receives the units already. D5b is
  what removes its second compile.

**Criteria.** (1) e2e, llvm row: the database holds an entry for the toolchain's `std.cppm`
whose `directory` is the std cache directory. (2) e2e: `emit --spec compile-commands` equals the
build's database apart from the work directory. (3) For D5b: `provides` of the `mcpp:std` units
names the std cache BMI, and `build-id` is present and stable across two runs.

### 3.6 C6: the location

By design: clangd finds `<root>/compile_commands.json` by its upward search, without
configuration. A symlink at that path redirects the write (`compile_commands.cppm:333-354`);
docs/01 states neither. **Repair**: docs/01 and its zh counterpart state the location, the
redirect, and §3.2's rule.

### 3.7 Not mcpp

- **xmake** is a reference in the report, not a subject: its temporary mapper files and GCC-only
  flags are its own. After §3.2 a shared root file holds mcpp's configuration alone, with a
  warning.
- **clangd with a GCC database** cannot resolve `import std`: clang reads no GCC BMI and cannot
  scan `bits/std.cc` under GCC's command (measured). An editor that wants clangd on a GCC-built
  project asks for the clang view of the same plan (`emit build-database --toolchain llvm@…`).

## 4. Issue #699

### 4.1 The report

mcppls builds its model from `mcpp emit build-database --format json`. On the five-member
GalTranslPP workspace, the member GPPGUI requests the host tool `Updater` of `gpp.updater`, whose
build runs a `lupdate` check that failed because the machine's Qt lacked qtdeclarative. `emit`
answered `MCPP_BUILD_DATABASE_PLAN_FAILED` without `data`, the four members that planned lost
their sets, and mcppls fell back to a guessed model on which clangd crashed. The issue asks for
the planned members' sets with an error per failed member, and for one of: a plan-only signal, a
build-program failure reported per member, or no `check` actions in host-tool builds under
`emit`. It calls both behaviours specified (R5.2, R2.5) and files a request.

### 4.2 Measurement

| Invocation (fixture: `good` plans; `bad`'s build program exits 1; `user` requests host tool `t` of `tool`, whose blocking check fails) | Exit | `data` | Diagnostics |
|---|---|---|---|
| `emit --workspace` over `good`, `bad`, `user` | 1 | absent | `PLAN_FAILED`: `bad: build.mcpp exited with 1 (build aborted)`; `user` never planned |
| `emit` in `user` | 1 | absent | `PLAN_FAILED`: `building host tool 'tool:t' failed: build failed` |
| `mcpp build` in `user` | 2 | | the same failure, correct for a build; verbose, the inner build shows the check `… -- /bin/false` failing |
| `emit --workspace` over `good` | 0 | `good/good` | none |

The member loop of `emit` stops at the first failure (`src/cli/cmd_build.cppm:352-380`), while
`mcpp build --workspace` continues past one ("continue-on-failure; first non-zero exit wins",
`:174-194`).

### 4.3 Classification

- **Not mcpp:** the environment (a Qt without qtdeclarative) and two project choices, a build
  program that verifies the environment and a blocking check inside a host tool's build. With a
  complete Qt the workspace plans all 227 units.
- **mcpp gap:** the unit of failure in `emit` is the whole command (R5.2), and a host tool's build
  is part of planning (R2.5).

### 4.4 E1: a member's failure costs that member (D1, accepted)

`emit` plans every selected member. A member whose planning fails contributes no sets and one
`error` diagnostic with the code R5.2 assigns today (`MCPP_BUILD_DATABASE_PLAN_FAILED` or
`MCPP_OFFLINE_DOWNLOAD_REQUIRED`) and `path` naming its `mcpp.toml`, relative to the workspace
root. `data` is present when at least one member was planned, and its `watch` also lists each
failed member's `mcpp.toml` and `build.mcpp`. The exit status is 1 whenever an error is present.
A consumer reads three outcomes structurally: no `data`; `data` with errors (described, except
what each error names); `data` without errors.

**S2 alignment.** S2-3.4-5 says `data` is present "when the command succeeded", and S2-3.4-11
that a command without `data` has failed; S2 has no partial outcome. One sentence in S2 §3.4
states it: `data` with `error` diagnostics is a document that describes everything except what
the errors name. `kindVersion` stays 1, which S2-3.4-3 requires.

**Criteria.** e2e on the fixture: the sets name `good/good`, one error per failed member with
`path` `bad/mcpp.toml`, exit 1; a workspace in which every member fails omits `data`.

### 4.5 E2: a host tool that does not build, under `emit`

Under `emit`, a host tool whose build fails is a warning, `MCPP_BUILD_DATABASE_HOST_TOOL_UNBUILT`,
naming the tool, its package and the first line of the failure. Planning continues, and the
build program receives the path the tool would be published at, which the store key fixes before
any build (`tool_store::bin_path`, `src/build/prepare.cppm:10342-10351`). A program that only
names the tool configures exactly as after a successful build; a program that runs it while
configuring (the pattern of docs/30's `dep_bin` example) fails, and E1 and E3 govern that
failure. The tool is still built, because a program may run it; its `check` actions still run,
because `blocking = true` is its author's statement, and a tool built without them would enter
the store unverified under the key a verified build uses. `mcpp build` is unchanged.

**Criteria.** e2e, member `user`: `emit` exits 0 with the member's sets and one warning; `mcpp
build` still exits 2.

### 4.6 E3: a build program that fails costs its own directives (D3, accepted)

Under `emit`, a package whose build program fails (does not compile, exits non-zero, times out,
or prints output mcpp refuses) is described without that program's directives, with one error,
`MCPP_BUILD_DATABASE_PROGRAM_FAILED`, whose `path` names its `build.mcpp`. The manifest's part of
the configuration, the toolchain, the module graph and the standard-library units are described
as usual; a failure that follows from the missing directives fails the member, and E1 applies.
Directives from a failed run are never applied (`src/build/build_program.cppm:1601-1640`), so no
half-applied state exists.

**Criteria.** e2e: a single package whose program exits 1, and one whose program does not
compile: `data` present with the package's sources, one error, exit 1.

### 4.7 E4: the plan-only signal (D4)

**What it is.** #699's first request is a signal, for example the environment variable
`MCPP_PLAN_ONLY=1` or a function `mcpp::plan_only()`, that tells a build program it runs for
`emit build-database` rather than for a build, so that it can skip checks and heavy work. **D4
is the decision not to add it**, for three reasons.

1. It gives a build program two behaviours, and the database describes one of them; nothing
   checks that the other emits the same directives, so R1.2 would hold only by each author's
   discipline (P6).
2. `--configure-only`, the reference R1.2 names, would have to run one of the two, and either
   choice breaks an identity.
3. The separation already has a place: verification is a `check` action, which `emit` does not
   run (P5), and R2 (§5.4) makes such a check cost nothing on a build whose inputs did not change.

### 4.8 Usage side

Environment verification is a `check` action, `blocking = true` where compilation must wait for
it; a program that cannot find something it would configure says so with `mcpp::warning` and
emits what it has. docs/30 states both.

## 5. #701 and #702

### 5.1 The proposal

#702 (2026.9.27.1, +494/-15, CI green except the two xcode-27 jobs of #669) adds:

1. `mcpp::runtime_library_dir(dir)`, wire `mcpp:runtime-library-dir=`, protocol 12, "the
   build-program form of `[runtime] library_dirs`": a new directive row
   (`Scope::LinkGlobal`, `Transform::AbsPath`, persisted in the build-program cache) whose
   `apply` appends to `RuntimeConfig::libraryDirs`, and the root package's residue mirrored into
   the plan snapshot as `deploy`'s is.
2. `__action-stamp` records each stamp's modification time before the command and, on success,
   moves a stamp the command did not write to the present.

The pull request is reused as this round's single pull request (D7): it is retitled and
re-implemented to §5.4.

### 5.2 Is the directive needed?

**What exists.** `[runtime] runtime_search_dirs` (and the retiring `library_dirs`) are static
lists relative to the package root; `deploy` places named files at paths relative to the program.
The readers: the merge into `LinkIntent::runtimeSearchDirs` (`src/build/plan.cppm:911-948`),
RUNPATH/rpath on ELF and Mach-O (`src/build/flags.cppm:507`), `mcpp run`'s loader path
(`src/build/execute.cppm:553`), `mcpp pack`'s closure search (`src/pack/pipeline.cppm:194-195`,
`:463-464`) and runtime validation (`src/build/runtime_validation.cppm:475`).

**The gap.** A directory whose location only a build program learns (a vcpkg install root, an SDK
found through `xpkg_dir`) and whose files do not exist when the program runs, because an
installation action creates them later. `deploy` needs the file names at plan time. The plugin's
route on 2026.9.26.1 is to deploy what exists at plan time, so the libraries a first installation
produces are placed by the next plan, which `mcpp run` performs (its record, D8, and
`deps/deps.cppm`).

**Two observations narrow the need.** On Windows, placement beside the program is the platform's
convention and the only form that also serves a program started by hand, since PE has no run path;
a search directory serves `mcpp run` and `mcpp pack` only. And the consumer ships without the
directive.

**Verdict.** The gap is real and general, a directory-level runtime search declared at
configuration time, and it is the build-program form of an existing key, as `include-dir` and
`deploy` are.

### 5.3 #702 as written

The directive follows the extension path the directive table defines: one row with protocol 12,
a non-empty cache tag replayed on a hit (the reasoning `warning` and `pack-format` state), the
root's residue mirrored, both languages of docs/04 and docs/30, unit and e2e tests. Four points do
not conform.

1. **It extends a retiring field (P8).** `RuntimeConfig::libraryDirs` is one of "the four legacy
   vectors … readable for one compatibility train" (`modules/manifest/src/types.cppm:1149-1150`);
   docs/04 §2.11 says the same of `library_dirs`, and the fingerprint names it
   `legacy-runtime-dir` (`src/build/prepare_inputs.cppm:723-725`). Its successor,
   `LinkIntent::runtimeSearchDirs`, reaches the same readers through the same `absolute_from`
   merge (`src/build/plan.cppm:911-920`) and accepts absolute paths, as `LinkIntent::deploy`
   already does for the `deploy` directive's absolute `from`. #702's docs say the directive joins
   `LinkIntent`; its code joins the legacy vector.
2. **The platform it is for has no criterion.** e2e 779 declares `# requires: pack gcc`, which
   holds on Linux only; `mcpp run`'s `PATH` and `mcpp pack`'s PE closure on Windows, the cases the
   issue names, are not exercised.
3. **The docs example** passes a Qt `bin/` to `link_search`; on Windows import libraries are in
   `lib/` and DLLs in `bin/`.
4. **No CHANGELOG entry**, which every version pull request since 2026.9.21.3 carries.

### 5.4 The conformant design

Four changes, each stated as the rule it implements. SPEC-007 (§5.6) states the same rules from
the plugin author's side.

**R1'. `mcpp::runtime_search_dir(dir)`: the build-program form of `runtime_search_dirs`.**

- Wire `mcpp:runtime-search-dir=<dir>`, protocol 12: a directive row with `Scope::LinkGlobal`,
  `Transform::AbsPath` and a non-empty cache tag. `apply` appends to
  `LinkIntent::runtimeSearchDirs`, and the root package's residue is mirrored into the plan
  snapshot as `deploy`'s is.
- The readers are the ones §5.2 lists, unchanged; a dependency's declaration reaches the
  consumer's executable through the same merge. The directory need not exist when the program
  runs, because a `prepare` action may populate it.
- `[runtime] library_dirs` keeps its legacy status and gains no directive.
- **Criteria.** Unit: the parse, the absolute-path transform, the protocol gate, the cache round
  trip, and the field it lands in. e2e on Linux: RUNPATH in `build.ninja`, `mcpp run`, a replay on
  a cache hit, `mcpp pack`. e2e on Windows: a DLL in a declared directory is found by `mcpp run`
  through `PATH` and placed beside the program by `mcpp pack`.

**R2. After a `check` or `prepare` command succeeds, every stamp is newer than every input.**
The engine creates each declared stamp that is missing and sets the modification time of each
existing one to the present, whether or not the command wrote it; on failure it writes nothing.
The rule needs no record of a stamp's time before the run. #702's form, which moves only a stamp
the command did not write, meets the same rule and is acceptable. A stamp feeds no compile or link
edge (a blocking check or a `prepare` action orders edges through an order-only edge), so touching
a stamp the command wrote changes no build. **Criterion:** e2e 780, which fails on 2026.9.26.1.

**P. The `prepare` role (O1).** An action whose command populates a directory that the build reads
by directory, and whose file names are not known when the build program runs: installing a vcpkg
manifest or a CMake subproject into a prefix, unpacking an SDK.

- **Outputs:** one or more stamps, which the engine writes as for `check` (R2), and one declared
  directory, `a.output_dir(dir)`, which the command populates. When the command succeeds and the
  directory does not exist, the engine writes no stamp and fails the edge, naming the directory.
- **R1.3 becomes a check.** A build program whose rerun inputs (`rerun_if_changed`,
  `rerun_if_changed_glob`) lie inside a declared `prepare` directory reads a construction result
  while it configures; the engine warns and names both. This is the pattern the plugins use today
  to place a first installation's libraries on the next plan.
- **Ordering:** every compile edge and the link edge of the declaring package wait for it. It is
  construction: a policy that concerns checks does not concern it, and the build's progress lines
  label it `PREPARE`.
- **References to its products** are names fixed at configuration time: `include_dir`,
  `link_search` and `runtime_search_dir` for directories, `link_flag` for a library's full path.
  The rule `src/build/hostprogram.cppm:187-194` states ("Content may arrive later; names may not")
  holds at the granularity of a directory.
- **Spelling:** `a.role = mcpp::roles::prepare;`. The constants
  `mcpp::roles::{source, check, object, artifact, prepare}` exist from protocol 12. An older
  engine's bundled module lacks them, so a build program that uses them fails to compile on that
  engine and names the constant, instead of the older engine reading the string `"prepare"` as
  `source`.
- **Unknown role strings are refused** from protocol 12, with the list of roles. Today
  `decode_action` maps any unknown string to `source`
  (`modules/buildmcpp/src/directives.cppm:1014-1018`) and `action_error` checks only the command
  and the outputs, so a misspelt role changes an action's meaning without a word.
- **Why a role and not `check` with `blocking = true`.** A role is an input to engine decisions.
  #699's third request, not adopted, was to skip the `check` actions of host-tool builds under
  `emit`; had it been adopted, every installation written as a check would have been skipped. A
  construction step labelled as verification is broken by the first decision made on its label.
- **Criteria.** Unit: the role decoded from the constant and from the string; an unknown string
  refused with the list. e2e: a `prepare` action populates a directory whose file names the build
  program does not know; a unit that includes a header from it compiles, and a program linked
  against a library in it runs through `runtime_search_dir`, on the first build; a second build
  runs nothing; after an input of the action changes, it runs once and not again. A `prepare`
  action whose command creates nothing fails naming its directory, and a build program that
  declares a rerun input inside that directory is warned.

**W. A Windows program's runtime libraries are placed beside it (O2).** A PE program has no run
path, so a DLL in a runtime search directory serves `mcpp run` (through `PATH`) and `mcpp pack`,
and not a program started by hand from the build directory. The platform's own convention is
placement: vcpkg's integration copies a program's imported DLLs beside it after the link, and
CMake names the same set `$<TARGET_RUNTIME_DLLS>`.

- **Rule.** After the link of a PE program whose plan has runtime search directories, an engine
  edge places beside the program every DLL the program imports, directly or through another DLL,
  that resolves in one of those directories. The resolution is `mcpp pack`'s:
  `read_closure` (`src/pack/pack.cppm:1323`), the directory order of `pipeline.cppm:193-202`, and
  the system rule that never copies an API set or a system DLL (`src/pack/binfmt.cppm:812-823`).
- **The edge** is engine-internal, as the check stamp's wrapper is: its inputs are the program and
  the stamps of the graph's `prepare` actions, it reports the DLLs it resolved in a depfile (the
  mechanism device compilers already use), so a later change to any of them runs it again, its
  output is a stamp, and a copy is replaced only when the source differs. Two directories that offer one DLL name are resolved in search order,
  and the choice is reported.
- **Nothing else changes:** ELF and Mach-O keep their run paths; `mcpp pack` is unchanged; a
  plan without runtime search directories has no such edge.
- **Criteria.** e2e on Windows: a DLL in a declared runtime search directory, populated by a
  `prepare` action, is found by the program started by hand from the build directory after the
  first build; replacing the DLL in that directory replaces the copy on the next build; a system
  DLL is never copied.

### 5.5 F1: `$ORIGIN` in a user link flag (mcpp-community/mcpp#703)

The plugin record reports that a build program's `link_flag` loses `$ORIGIN` through "ninja and
the shell". Measured on 2026.9.26.1 (gcc@16.1.0), for `mcpp::link_flag("-Wl,-rpath,$ORIGIN/../lib")`
and for `[build] ldflags = ["-Wl,-rpath,$ORIGIN/../lib"]` alike:

```
build.ninja:  -Wl,-rpath,$$ORIGIN/../lib
program:      (RPATH) [<glibc lib64>:<gcc lib64>:/../lib:<subos lib>]
```

**Cause.** SPEC-004 §8 states how an element of `cflags`, `cxxflags` and `asmflags` (and of
their directives) becomes words, forbids an implementation to interpret `$` (rule 7), and requires
every word to reach the compiler verbatim whatever the host's command-line reader. `ldflags` and
the link directives have no such statement, and `normalize_ldflag`
(`src/build/flags.cppm:367-385`) escapes an element for ninja only, so the `sh` that runs the
command expands `$ORIGIN` to nothing. The engine's own run path is rendered
`-Wl,-rpath,'$$ORIGIN'` (`src/build/plan.cppm:745`, `:2125`), quoted for ninja and for the shell.
The result is worse than a lost entry: `/../lib` is the host's `/lib`, a host directory in the
program's run path, the class #696 closed for links.

**Repair.** SPEC-004 §8's reading applies to `ldflags` and to the link directives: an element is
read into words, and each word reaches the linker verbatim, escaped for ninja and quoted for the
host. The same reading splits an element that packs several tokens, which the rendering of
link-unit flags guards against case by case today (`src/build/ninja_backend.cppm:342-370`).
**Compatibility:** an element written for the shell or for ninja by hand (`\$ORIGIN`,
`'$$ORIGIN'`) changes meaning; the index is searched for such spellings before release. Windows is
unaffected (no shell). **Criterion:** e2e, `$ORIGIN` from the manifest and from `link_flag`
reaches the program's run path verbatim (fails on 2026.9.26.1). Filed as #703.

### 5.6 SPEC-007: the build plugin specification

`docs/specs/build-plugins.md` (SPEC-007, draft 0.1, written in Chinese as SPEC-001 to SPEC-006
are) is the contract for rule packages, dependency adapters and distribution members. Each rule
carries the implementation state the spec index defines, and a rule no engine check enforces is
marked as an author's obligation.

| § | Content |
|---|---|
| 1 | Three kinds of work and the mechanism of each: configuration (directives), construction (actions), verification (`check`). Construction is never done while the build program runs; environment incompleteness is a warning, never a non-zero exit; configuration depends only on declared inputs and never on construction results; one behaviour under planning and building |
| 2 | Which directive expresses which configuration (including `runtime_search_dir`); package-relative paths; no host system directories; no retiring fields; link flags read as words (#703) |
| 3 | Actions as argv with the tool as an input; outputs named at submission; the five roles with `prepare`; `check` only for verification; the stamp rule R2; role constants; network access belongs to installation, and an offline build (`MCPP_OFFLINE=1`, inherited by actions, `src/cli.cppm:167-172`) does not reach the network |
| 4 | Runtime search: `runtime_search_dir` for a directory, `deploy` for a file known at configuration time, what Windows provides, no run path through `link_flag` |
| 5 | Obligations under planning: E2 and E3 from the plugin's side |
| 6 | Payloads declared where the lookup happens; no probing of host paths |
| 7 | The engine release a plugin needs, stated; the index CI pin moves and `min_mcpp` does not |
| 8 | A criterion on every platform claimed; a criterion that fails before its change; a planning criterion |

**What it asks of mcpp-plugins 0.13** (the feedback for the plugin side):

| Plugin | Today (its design record, D8) | Under SPEC-007 |
|---|---|---|
| `deps-vcpkg`, `deps-cmake` | the installation is a blocking `check` | a `prepare` action (R3.3, R3.4) |
| `deps-vcpkg`, `deps-cmake` | the prefix's shared libraries are enumerated at plan time and deployed one by one, so a first installation's libraries are placed by the next plan | `runtime_search_dir` for the prefix's `bin/` (Windows) or `lib/` (R4.1); no enumeration of construction results (R1.3, now warned); on Windows, W places the DLLs beside the program after the first build |
| `deps-vcpkg` | vcpkg may download during the build | stated in its documentation; an offline build completes from vcpkg's caches or fails naming what is missing (R3.7) |
| `rules-qt` | Windows: the DLLs of the linked modules and of the modules they depend on, deployed beside the program | conforms as it is: the SDK is a payload installed before the build, so these files are known at configuration time (R1.3, R4.2). After W, `runtime_search_dir` for the SDK's `bin/` gives the same result without the plugin computing the module closure |
| `rules-qt` | Linux: glib, zstd and zlib linked by full path with `--no-as-needed` | unchanged: QtCore's own `DT_RUNPATH` stops the program's run path from applying to QtCore's dependencies, a loader rule rather than an engine gap; `$ORIGIN` in link flags is usable after #703 (R2.4) |
| all | engine floors written as versions | the first mcpp release that speaks protocol 12, stated in each plugin's documentation (R7.1) |

**O2** (a program started by hand on Windows) is designed in §5.4 as W, a general mechanism
(P9), and SPEC-007 R4.3 states it.

### 5.7 Disposition

#702 is reused as this round's single pull request (D7): retitled, and re-implemented to R1', R2,
P and W, with F1 and the rest of §7 in the same pull request. mcpp-plugins adopts SPEC-007 after the
release that carries protocol 12.

## 6. Specifications and documentation

| Document | Change |
|---|---|
| SPEC-005, to v1.3 | R2.5: a host tool that does not build under `emit` is a warning (E2). R3.7: `work-directory` is the directory the compiler runs in; `arguments` include the interface language flag (C3, C4). R3.8: `provides` of the standard-library units names the std cache BMI (D5b). R4.1: the compile-commands document includes the standard-library units (C5, S1-12-1). R5.2: member containment and the program-failure description (E1, E3). |
| SPEC-004 §8 | the element reading extends to `ldflags` and the link directives (F1, #703) |
| SPEC-007, new (draft 0.1) | `docs/specs/build-plugins.md`, and its row in `docs/specs/README.md` |
| docs/50 §8, and zh | the failure paragraph; `MCPP_BUILD_DATABASE_HOST_TOOL_UNBUILT` (warning), `MCPP_BUILD_DATABASE_PROGRAM_FAILED` (error); `path` on member errors |
| docs/01, and zh | the root file and the configuration's database (§3.2), the symlink redirect, the standard-library units |
| docs/30, and zh | configuration, construction and verification (§4.8, SPEC-007 §1); `dep_bin` under `emit`; `runtime_search_dir`; the `prepare` role and the role constants; the stamp rule |
| docs/31, and zh | the role table gains `prepare`; a pointer to SPEC-007 |
| docs/04 §2.11, and zh | the build-program form of `runtime_search_dirs`; the PE column of the link-intent table gains W's placement |
| S2 §3.4 (mcppls repository) | one sentence: `data` with error diagnostics (E1) |
| CHANGELOG | one entry per change |

## 7. Plan

Every item lands in #702, retitled.

| # | Change | Where | Tests |
|---|---|---|---|
| W1 | §3.2: the configuration's database, the root copy, the warning, the fast-path restore, the `.gitignore` | `src/build/compile_commands.cppm`, `src/build/ninja_backend.cppm`, `src/build/execute.cppm`, `src/scaffold/create.cppm` | e2e (four), unit |
| W2 | C3 and C4 in `UnitInvocation` | `src/build/compile_commands.cppm` | unit; e2e replay (gcc, llvm); e2e 211 |
| W3 | C5: the plan carries the standard-library description; both databases render it; D5b's `provides` and `build-id` | `src/build/plan.cppm`, `src/build/prepare.cppm`, `src/build/compile_commands.cppm`, `src/build/build_database.cppm` | e2e |
| W4 | E1 | `src/cli/cmd_build.cppm`, `src/build/build_database.cppm` | e2e |
| W5 | E2 | `src/build/prepare.cppm` (host-tool branch, `plan_only` only) | e2e |
| W6 | E3 | `src/build/prepare.cppm` (both `run_build_program` sites, `plan_only` only); a severity on `PlanNote` | e2e (two) |
| W7 | R1' and R2 | `modules/buildmcpp/src/directives.cppm`, `modules/buildmcpp/src/program_protocol.cppm`, `src/build/hostprogram.cppm`, `src/build/prepare.cppm`, `src/cli.cppm` | unit; e2e on Linux and Windows; e2e 780 |
| W8 | F1 (#703): SPEC-004 §8's reading for link flags | `src/build/flags.cppm`, `docs/specs/manifest-semantics.md` | unit, e2e |
| W9 | P: the `prepare` role with `output_dir`, the role constants, the refusal of unknown roles, the R1.3 warning | `modules/buildmcpp/src/directives.cppm`, `src/build/hostprogram.cppm`, `src/build/ninja_backend.cppm` (stamp wrapper, directory post-condition, ordering including the link edge, the #534 self-check, the progress label), `src/build/prepare.cppm` (the warning) | unit, e2e |
| W10 | W: runtime DLL placement after a PE link | `src/build/ninja_backend.cppm` (the edge), `src/cli.cppm` (its engine-internal command), `src/pack/pack.cppm` (`read_closure` reused) | e2e on Windows |
| W11 | §6, SPEC-007 included | specifications, docs, CHANGELOG; S2 in the mcppls repository | docs checks |

W7 and W9 share one protocol bump, to 12; W10 follows W7 and W9, whose directories it reads. W1 and W2 are tested together (W1's criteria read
`directory`). The version is the next date version at release time. After the release, mcppls
reads `data` with errors and may use D5b's reuse, and mcpp-plugins moves to SPEC-007.

**Cross-platform.** C3 on Windows is the native spelling of `plan.outputDir`. W1 copies rather
than links the root file, since a symlink needs a privilege on Windows. W7 carries a Windows leg.
W9 is platform-independent: an action is an argv on every host. W10 exists on Windows only, and
its criterion runs there. C4's MSVC form and C5 on MSVC are
open measurements. F1 is POSIX-only. E1 to E3 do not depend on the platform.

## 8. Decisions

| | Decision | State |
|---|---|---|
| D1 | E1 as the default | **accepted** |
| D2 | C3 for every toolchain | **accepted** |
| D3 | E3 as the default | **accepted** |
| D4 | no plan-only signal | **accepted** |
| D5 | D5a: standard-library units in `compile_commands.json`; D5b: their BMI in S1 `provides`, and `build-id` | **accepted** |
| D6 | one database per configuration | **accepted** |
| D7 | #702 reused as the single pull request and re-implemented to §5.4 | **accepted** |
| D8 | F1 repaired by extending SPEC-004 §8 to link flags; filed as #703 | **accepted** |
| D9 | P: the `prepare` role with its declared directory, the role constants, the refusal of unknown roles, and the R1.3 warning, in this round | **accepted** |
| D10 | R2 in its simple form (every stamp touched on success) | **accepted** |
| D11 | W (O2): the engine places a Windows program's imported DLLs beside it after the link | **accepted**, with the Windows e2e as its criterion |
| D12 | SPEC-007 draft 0.1 as the contract relayed to mcpp-plugins | **accepted** |

## 9. Risks and open measurements

- C4 and C5 on MSVC (Windows CI): `/interface` in a clang-cl-mode clangd; an MSVC STL `std.ixx`
  unit.
- C5's cost for a matching plain clangd: one `std` build per session.
- §3.2 replaces a root file another tool shares with mcpp, including through a root symlink into
  that tool's directory; the warning makes it visible.
- E3: code after a build program in `prepare_build` may assume its directives were applied; each
  such failure fails the member under E1, and the e2e set includes one.
- E2 retries a failing tool build on every `emit`, as today's failure does.
- F1 changes the meaning of a link flag escaped for the shell or for ninja by hand.
- P on an engine older than protocol 12: a build program that spells the role with a constant
  fails to compile and names it; one that writes the string `"prepare"` is read as `source` by
  that engine. SPEC-007 R3.6 requires the constant.
- deps-vcpkg's downloads at build time (SPEC-007 R3.7) depend on how vcpkg's caches are populated;
  the plugin states its answer.
- W copies DLLs into the build directory on every PE build that has runtime search directories;
  a copy is replaced only when its source differs, and its criterion runs on Windows CI only,
  where it is the first measurement of the closure walk outside `mcpp pack`.
- The R1.3 warning appears on the first build after the upgrade for the plugins that use the
  second-plan route today; SPEC-007 tells them what replaces it.
- E1 depends on one sentence in S2.

## 10. Overall review

- **Architecture.** One record serves both databases (P1). One directory names a configuration,
  and it is the directory the compiler runs in (C3, §3.2), so the identifier the merge needs and
  the fact the specifications ask for are one value. Configuration, construction and verification
  each have one mechanism (SPEC-007 §1), and the one gap in that table, construction whose file
  names are unknown, gets a role instead of a borrowed one (P). New surfaces join the current model
  (R1', P8). Every engine change is general and names no tool (P9); what the plugins knew and
  worked around, the engine now states once (R1', P, W).
- **Consistency with the specifications.** C3 meets the JSON format and S1-8-2; C5 meets S1-12-1;
  D5b meets S1-8-6 and S1-11.2-3; E1 needs one sentence in S2; F1 extends SPEC-004 §8 to the one
  flag list it did not cover. No field outside the JSON format is written into
  `compile_commands.json`, which its readers reject (measured).
- **Compatibility.** Behaviour a user can see changes in eight places: the root database is
  replaced rather than merged, with a warning when another writer's entries go; `directory` names
  the output directory; interface entries carry their language flag; standard-library entries
  appear; `emit` returns partial documents with errors; `$ORIGIN` in a link flag reaches the linker;
  an unknown action role is refused instead of read as `source`; on Windows, a program's DLLs from
  its runtime search directories are placed beside it. Each follows a specification or an
  accepted decision. The `emit` envelope keeps `kindVersion` 1. Protocol 12 is additive: a build
  program that uses none of its surfaces behaves as before, and one that uses them fails to compile
  on an older engine and names what it lacks.
- **Stability.** The fast path gains a copy, not a plan. No compile changes. W1 to W6 change
  descriptions. A link changes where W8 corrects a run path that is wrong today, and where a build
  program declares a runtime search directory. R2 changes a stamp's time. P adds an ordering that
  the declaring package asked for.
- **User experience.** An editor keeps every member that plans (E1), a package whose program fails
  (E3), and a member whose host tool does not build (E2). A deleted database returns on the next
  build. A dependency installed by a plugin is usable on the first build, without the second plan
  the plugin needs today, and on Windows a program started by hand finds its DLLs (W). Verification in `check` actions costs nothing when its inputs did not
  change (R2).
- **Test coverage.** Each item has a criterion that fails on 2026.9.26.1 (C1, C3 for gcc, C4, E1,
  E2, E3, R2, F1) or states a new property (§3.2's four e2e, C5, D5b, R1' on Linux and Windows, P).
  W has its criterion on Windows. SPEC-007 §8 asks the same of every plugin, including a planning
  criterion.
- **Measured and not measured.** Measured: every reading in §3, §4.2, §5.5 and Appendix A. Not
  measured: MSVC (C4, C5), macOS, clangd 23.1 itself (its mechanism is reproduced with llvm@20.1.7
  and clangd 22.1.8), Meson (its binary on the measuring host does not run), and W, whose criterion
  runs on Windows CI.
- **Corrections made while writing.** C1 had been recorded twice before (#397 C-1, #677 B1).
  Listing the standard-library units was first expected not to help; clangd builds from a listed
  provider. GCC was first expected to hand `.ixx` to the linker; GCC 16 compiles it. Revision 1's
  four-condition merge kept a merge across configurations; §3.2 removes it. The first draft of the
  plugin migration table moved `rules-qt` on Linux to `runtime_search_dir`; QtCore's own
  `DT_RUNPATH` makes the program's run path irrelevant to QtCore's dependencies, so that row stays
  as the plugin has it.

## 11. Implementation

### 11.1 Tasks

| Task | Items | Branch | Owns | e2e |
|---|---|---|---|---|
| T1 the compile database | W1, W2, W3 | `feat/702-cdb` | `compile_commands.cppm`; the fast path in `execute.cppm`; `scaffold/create.cppm`; the standard-library units, `provides` and `build-id` in `build_database.cppm`; the writer's call site in `ninja_backend.cppm` | 781-786 |
| T2 `emit` | W4, W5, W6 | `feat/702-emit` | the member loop in `cmd_build.cppm`; member diagnostics in `build_database.cppm`; the host-tool and build-program branches of `prepare.cppm` under `plan_only` | 787-789 |
| T3 build-program surfaces | W7, W9 | `feat/702-actions` | `directives.cppm`, `program_protocol.cppm`, `hostprogram.cppm`; `__action-stamp` in `cli.cppm`; the action edges in `ninja_backend.cppm`; the residue mirror and the R1.3 warning in `prepare.cppm` | 779, 780, 790-794 |
| T4 link flags | W8 | `feat/702-link` | `normalize_ldflag` in `flags.cppm`; the link-flag rendering in `ninja_backend.cppm`; SPEC-004 §8 | 795-796 |
| T5 DLL placement | W10 | `feat/702-link` | the placement edge in `ninja_backend.cppm`; its internal command in `cli.cppm`; the closure walk shared with `mcpp pack` | 797-798 |
| T6 documents and release | W11 | `feat/runtime-library-dir` | specifications, docs and their zh pairs, CHANGELOG, the version, the xlings pin | |

### 11.2 Dependencies

- T1 to T4 are independent and proceed in parallel, each in a worktree from #702's head.
- T5 reads `LinkIntent::runtimeSearchDirs`, which `[runtime] runtime_search_dirs` already
  feeds, so it proceeds in parallel against the manifest key; T3's `prepare` stamps join its
  inputs at integration.
- The shared files are `prepare.cppm` (T1, T2, T3), `ninja_backend.cppm` (T1, T3, T4, T5),
  `cli.cppm` (T3, T5) and `build_database.cppm` (T1, T2); each task changes its own functions.
  Integration merges T3 first (it moves the protocol), then T4 and T5, then T1, then T2.
- T6 follows all five: each task reports its specification and documentation text.

### 11.3 Cross-repository sequence

1. mcpp #702 carries T1 to T6, the version and the xlings pin; every CI leg is green except the
   xcode-27 legs of #669.
2. Before the release, the index and mcpp-plugins are searched for action roles outside the
   five (P refuses them) and for link flags that escape `$` by hand (F1 changes their meaning).
3. The release: `release.yml`; each archive is uploaded to GitCode with the local gtc as it
   appears; the xim-pkgindex bump is merged by a maintainer; the index's `latest` is read back.
4. The ecosystem: the published mcpp is verified in an xlings sandbox with the CN mirror
   (`xlings subos use <n> --sandbox --cmd ...`): installation, `new`/`build`/`run`, the compile
   database, `emit` over a workspace with a failing member, a build program with
   `runtime_search_dir` and a `prepare` action, and index packages including the rules plugins.
5. mcppls: S2 §3.4's sentence on `data` with error diagnostics, in its repository.
6. mcpp-plugins: SPEC-007 is relayed by the maintainer, and the plugins move after the release.
7. #699, #701 and #703 are closed with the release's readings.

### 11.4 Review axes

| Axis | What holds it |
|---|---|
| Architecture | one record for both databases (P1); one directory names a configuration; one mechanism per kind of work (SPEC-007 §1); engine changes name no tool (P9) |
| Stability | the fast path gains a copy and no plan; no compile command changes; links change only where F1 corrects a run path or a program declares a runtime search directory |
| Simplicity | one function publishes the root file on both paths; one stamp rule for both stamped roles; one role table decodes constants and strings |
| User experience | an editor keeps every member that plans; a deleted database returns; a first installation is usable on the first build; a Windows program started by hand finds its DLLs |
| Compatibility and upgrade | protocol 12 is additive; `kindVersion` stays 1; cache entries of older programs replay unchanged; a foreign database and a rerun input inside a `prepare` directory are warnings; the one new refusal, an unknown role, is searched for in the index before the release |
| Cross-platform | C3 in native spelling; the root file copied, never linked; R1' with a Windows leg; W on Windows CI; C4 and C5 measured on MSVC before their MSVC form is emitted |
| Consistency | SPEC-004, SPEC-005, SPEC-007, docs/01, docs/04, docs/30, docs/31, docs/50 and their zh pairs change in the same pull request as the code |
| Test coverage | each item has a criterion that fails on 2026.9.26.1 or states a new property; the full e2e suite runs on the integration branch before the pull request is pushed |

## Appendix A. Measurement record

All runs use `~/.xlings/data/xpkgs/xim-x-mcpp/2026.9.26.1/bin/mcpp`, clangd and clang-tidy from
`~/.xlings/data/xpkgs/xim-x-llvm-tools/22.1.8/bin`, and scratch directories.

### A.1 The compile database (a copy of `hello`)

```
report 3.6: rm compile_commands.json; mcpp build
  last line: Finished dev in 0.00s; compile_commands.json NOT regenerated; after --no-cache: present
report 3.1: xmake's sample moved to the copy, touch src/main.cpp, mcpp build
  from the root: 6 entries (g++ bits/std.cc; clang++ greet.cppm, main.cpp, test.cppm; g++ src/greet.cppm, src/main.cpp)
  from src/:     4 entries (the two relative ones pruned)
report 3.2: replay with -o redirected, interfaces first
  gcc  from the output directory: 3 of 3 rc=0, objects written
  gcc  from directory: greet.cppm rc=0; test.cppm rc=1; main.cpp rc=1; gcm.cache/ written into the project root
  llvm from either: 3 of 3 rc=0
two toolchains: mcpp test (llvm), then mcpp build --toolchain gcc@16.1.0
  g++ greet.cppm | g++ main.cpp | g++ test.cppm | clang++ tests/test_smoke.cpp
configurations: target/x86_64-linux-gnu/ holds two fingerprint directories after build, test and
  --no-cache runs with one toolchain and a build with the other; after `mcpp build` and `mcpp test`
  in one configuration, the three build entries and the test entry all name one directory (6413abdf...)
```

### A.2 clangd and clang-tidy

```
.ixx without -x c++-module: E [fe_expected_compiler_job] ... expected exactly one compiler job
.ixx with the flag:          All checks completed, 0 errors
llvm@20.1.7 database:        Failed to build module std; due to Don't get the module unit for module std
                             [ast_file_version_too_old] ... std.pcm; 1 error
+ mcpp:std units:            Built module std; Built module hello.greet; 0 errors
gcc@16.1.0 database:         module 'std' not found; 1 error (unchanged with bits/std.cc appended)
missing directory:           clangd: VFS: failed to set CWD ...; 0 errors
                             clang-tidy: LLVM ERROR: Cannot chdir into "..."! (exit 134)
one unknown key in an entry: clangd: Failed to load compilation database; clang-tidy: cannot load it
GCC 16 on .ixx:              g++ -std=c++23 -fmodules -c m.ixx: rc=0, object written
```

### A.3 #699

```
emit --workspace (good, bad, user): rc=1, no data, PLAN_FAILED bad: build.mcpp exited with 1 (build aborted)
emit in user:                       rc=1, no data, PLAN_FAILED building host tool 'tool:t' failed: build failed
mcpp build in user (verbose):       [1/5] mcpp __action-stamp .../lupdate.stamp -- /bin/false; FAILED
emit --workspace (good):            rc=0, sets ['good/good']
```

### A.4 `directory` in other producers

```
CMake 4.4.2, Ninja:           directory <build>;     file <src>/lib/lib.cpp; replay: object written
CMake 4.4.2, Unix Makefiles:  directory <build>/lib; file <src>/lib/lib.cpp; replay: object written
ninja -t compdb, mcpp graph:  directory <project>/target/x86_64-linux-gnu/<fingerprint>
```

### A.5 F1

```
mcpp::link_flag("-Wl,-rpath,$ORIGIN/../lib") and [build] ldflags, gcc@16.1.0:
  build.ninja: -Wl,-rpath,$$ORIGIN/../lib
  program RPATH: [.../xim-x-glibc/2.44/lib64:.../xim-x-gcc/16.1.0/lib64:/../lib:.../subos/default/lib]
```
