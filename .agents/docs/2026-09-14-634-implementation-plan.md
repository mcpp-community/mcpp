---
subject: triage
status: active
---

# #634 implemented across five repositories: the plan, its review, and the ledger that tracks it

**Status:** active. This record turns the decisions of
`2026-09-14-634-cmake-parity-items-by-home.md` (revision 2, "the triage
record" below) into work: one pull request per repository, the order in which
they merge and release, the criteria each task is held to, and a ledger whose
rows are closed only by a reading. §1 reviews revision 2 from the angles an
implementation adds and states the refinements adopted before any code was
written (revision 3). A refinement found while implementing is appended to §1
and folded into the triage record's §2 before the engine pull request merges.

Base commits: mcpp `b8d96844` (2026.9.14.1), mcpp-plugins `9301832` (0.9.3),
xim-pkgindex `19e264e6`, mcpplibs/mcpp-index `f68512d`, xlings `59068d6`.
HuxerUI and Lib-Live2D are not modified by this work; their changes are listed
for the project in the reply on #634.

## 0. The ledger

Status is one of `todo`, `doing`, `branch` (implemented on the pull request's
branch, with the reading that measured it), `done` (merged, with the reading
that closed it), `dropped` (with the reason). Owner `lead` is the integrating session;
`W1` to `W4` are the parallel work trees of §8.

### 0.1 Engine: mcpp-community/mcpp, one pull request (`feat/634-cmake-parity`)

| id | task (triage §) | owner | depends on | status |
|---|---|---|---|---|
| E1 | a matching conditional dependency declaration replaces the unconditional one, compared by identity; the same for `dev-dependencies`, `build-dependencies`, `feature-deps` (§5.1.1) | lead | - | branch: e2e 677 A, B (fails on 2026.9.14.1) |
| E2 | conditional dependency tables labelled by their full section; an option-named selector with a string value warns with the restated form (§5.1.2) | lead | - | branch: e2e 677 C |
| E3 | `[target.<sel>.targets.<n>] kind` for declared library targets; the `[target.<sel>]` sweep reports sub-tables the parser does not read (§5.1.3) | lead | - | branch: e2e 677 D, 678 D; unit `TargetScalarKeys.EveryParsedSubTableIsKnownToTheSweep` |
| E4 | the link-form degradation names the package's statement and, for a row, its selector (§5.1.3, §9 item 5) | lead | E3 | branch: e2e 678 C |
| E5 | a `path`/`git` dependency adopts its manifest's identity and warns once; the record is registered under both keys (§5.2) | lead | - | branch: e2e 679 A, B |
| E6 | two identities over one canonical source are refused before scanning; the scanner's duplicate-provider message names the packages (§5.2) | lead | E5 | branch: e2e 679 C |
| E7 | the six edges in mcpp's `examples/` write the declared identity (§5.2) | lead | E5 | branch: the three examples build with no identity warning, and still build on 2026.9.14.1 |
| E8 | one reader-driven closure for PE, the Android rows and Mach-O, with per-format platform rules (§5.3.1) | W1 | - | branch: unit `test_pack_closure` (11 cases); e2e 667, 668 (macOS CI) |
| E9 | Android stages `lib/` (per ABI), Mach-O stages beside the program; `walked` only when complete, else `not-walked` naming the names and `dir`/`tar` refuse; `needs` lines on every row (§5.3.2-5) | W1 | E8 | branch: e2e 266, 666, 668 (macOS CI), 667, 669 |
| E10 | rpath entries that begin with a loader token (`$ORIGIN`, `@executable_path`, `@loader_path`, `@rpath`) are not anchored, through one function shared by both normalisers (§9 item 11) | W1 | - | branch: unit `test_build_flags`; e2e 670 |
| E11 | an ELF shared library without a declared `soname` is linked with its file name as SONAME (§5.4) | W1 | - | branch: e2e 667, 669 |
| E12 | the static C++ runtime archives are located by asking the driver for the effective target (§5.6.1) | W2 | - | branch: e2e 675 |
| E13 | every runner of `mcpp run` and `mcpp test` receives `MCPP_RUNTIME_FILES` (§5.6.2, format refined in §1) | W2 | - | branch: e2e 672; the `android` job's emulator step |
| E14 | `run`, `test` and `pack` declare `--toolchain` (§5.10) | W2 | - | branch: e2e 671 |
| E15 | `[test] discover` (§5.5) | W2 | - | branch: e2e 673; unit `test_test_targets` |
| E16 | `mcpp run --format <f>` uses the named runner `<f>` when one exists; a directory distributable that meets no runner is refused before the spawn (§6.3) | W2 | - | branch: e2e 674 |
| E17 | two emissions of one named runner: measured, then decided (§9 item 8; see §1) | W2 | - | branch: decided in §1.9, e2e 684 |
| E18 | `resolution.json` records `graph`; `mcpp why deps` prints it (§5.11) | lead | E1-E6 | branch: e2e 682; 677, 678 and 679 read `graph` |
| E19 | the engine states `android.api-level`, `ios.deployment-target`, `macos.deployment-target`; the refusal names the fact's key and says "this build targets" (§5.9, §9 item 6) | lead | - | branch: e2e 680 |
| E20 | `config.toml [index.repos.<name>]` reaches an existing registry; a payload installed from an overridden index names the source (§7.4, §9 item 7) | lead | - | branch: e2e 681; an existing home re-pointed its index and restored the entry (local probe, §1.9) |
| E21 | `mcpp::pkg_config_libdir()` (§5.7) | lead | - | branch: e2e 683 |
| E22 | CI: the `android-ndk` e2e tests run on a Linux job, and one emulator step runs `mcpp test` on the x86_64 Android row (§1, test coverage) | lead | E9, E12, E13 | branch: `ci-linux-e2e.yml` job `android` |
| E23 | user documentation and its Chinese mirror; normative specification changes (§5 below) | lead, W1, W2 | E1-E21 | doing |
| E24 | triage record revision 3 folded in; this ledger closed for the engine rows; CHANGELOG; version group 1 | lead | E1-E23 | doing |

### 0.2 Payloads: openxlings/xim-pkgindex, one pull request (`feat/634-runners-and-payloads`)

| id | task (triage §) | owner | depends on | status |
|---|---|---|---|---|
| X1 | `macapp-run`: a macOS application bundle runner (§7.1) | W3 | - | done: xim-pkgindex#838 (8f67d875), job `macapp-run` |
| X2 | `simctl-run` spawns the installed executable of a bundle that does not load UIKit; UNMEASURED notes replaced by readings (§7.2) | W3 | - | done: xim-pkgindex#838, job `simctl-run` |
| X3 | `adb-run` transfers the files `MCPP_RUNTIME_FILES` names and runs the program from their directory (§5.6.2) | W3 | E13 merged in the branch | done: xim-pkgindex#838, job `adb-run` on the API 34 emulator |
| X4 | `xim:wix` close-out: anchor `mbanative.dll`, a Windows install-verify job, `WixToolset.BootstrapperApplications.wixext`, the ABI statement (§7.3) | W3 | - | done: xim-pkgindex#838, job `wix` |
| X5 | `xim:bundletool` (§6.5) | W3 | - | done: xim-pkgindex#838, workflow `bundletool` on three hosts |
| X6 | `libxml2` declares its `.pc` files into the view (§5.7, §9 item 9) | W3 | - | done: xim-pkgindex#838, job `consumers` |
| X7 | the pull-request workflow builds a consumer fixture through the `config.toml` index override (§7.4) | W3 | - | done: xim-pkgindex#838, workflow `consumer-through-index-override` |

### 0.3 Official plugins: mcpp-community/mcpp-plugins, one pull request (`feat/634-closure-bundles`, 0.10.0)

| id | task (triage §) | owner | depends on | status |
|---|---|---|---|---|
| P1 | `dist-apk` reads the staged closure (`lib/`, `lib/<abi>/`), deletes its walk and stamp, packs several ABIs into one APK, reports refusals through `mcpp::warning`, and refuses a stage without `needs` lines naming the engine floor (§6.5) | W4 | E9 | doing |
| P2 | `dist-apple` places staged dylibs in the framework directory, adds the link-time rpath through `mcpp::link_flag`, signs ad hoc when no identity is given, and keeps closure members out of the resource directory (§6.1) | W4 | E9, E10 | doing |
| P3 | `dist-apple` supplies `mcpp::runner("app", "macapp-run")` on `*-macos` and declares the payload (§6.3) | W4 | E16, X1 | doing |
| P4 | `dist-apple` `dmg` format (§6.2) | W4 | - | doing |
| P5 | `dist-apk` `aab` format (§6.5) | W4 | P1, X5 | doing |
| P6 | `dist-wix` `setup` format (§6.4) | W4 | X4 | doing |
| P7 | `rules-metal` (§6.6) | W4 | - | doing |
| P8 | CI: the iOS fixture exits non-zero, the diagnostic is portable, P1-P7 have jobs, `MCPP_VERSION` names the released engine (§7.2, §9 item 10) | W4 | R1 | doing |
| P9 | documentation, version 0.10.0, release, GitCode assets (§4 below) | lead | P1-P8 | todo |

### 0.4 Index: mcpplibs/mcpp-index, one pull request

| id | task | owner | depends on | status |
|---|---|---|---|---|
| I1 | `mcpp.plugins` 0.10.0 descriptor entry and its note | lead | R2 | todo |
| I2 | D1, HuxerUI's six-row descriptors | project | HuxerUI's release | dropped: follows the project's release, and HuxerUI is not modified here |

### 0.5 Release, verification, reply

| id | task | depends on | status |
|---|---|---|---|
| R1 | engine: merge, tag, `release.yml` on four hosts, `publish-ecosystem`, the bot's index pull request merged, GitCode assets checked (local `gtc` for any missing), bootstrap pin (version group 2) | E24, CI green | todo |
| R2 | plugins: merge, tag `v0.10.0`, release assets on GitHub and GitCode (`gtc`), byte comparison of each downloaded asset | P9, CI green | todo |
| R3 | xim-pkgindex pull request merged, and its artifact read back from a client | X1-X7, CI green | done: a local client installed `xim:android-platform-tools@37.0.1-4` from the index after the merge |
| R4 | mcpp-index pull request merged; `latest` of `mcpp.plugins` read back from a client | I1 | todo |
| V1 | sandbox verification of the released engine (§6.2) | R1, R3 | todo |
| V2 | sandbox verification of plugins 0.10.0 through the index (§6.2) | R2, R4 | todo |
| V3 | the ecosystem review (§7) | V1, V2 | todo |
| V4 | the reply on #634 | V3 | todo |

## 1. Review of revision 2 from the implementation's side, and revision 3

Revision 2 held each decision to six properties. An implementation adds
angles the triage could not: what a later reader of each new record needs,
what an engine one release older does with a manifest written for this one,
which rows CI can reach, and where a test would stay green over a regression.
Each angle below names what it checked and what it changed.

### 1.1 Architecture

- **E1 compares identities, not map keys.** The dependency maps are keyed by
  `selector.stableMapKey` (`toml.cppm:1509`); an unconditional `fw` and a
  conditional `mcpplibs.fw` are one identity under two keys. Replacement
  therefore removes every unconditional entry whose normalised identity equals
  the conditional entry's before inserting it. A map-key comparison would
  leave both entries, and the resolver would see two declarations of one
  package.
- **E8 keeps one function and one data flow.** The closure function takes the
  artifact, a list of search directories and a platform predicate, and
  returns resolved members, platform names and unresolved names. The three
  rows differ only in the predicate and in the stage layout; the manifest
  writer, the refusal and the `needs` lines are shared. The ELF host row keeps
  `ldd_parse` (triage §5.3) and feeds the same writer.
- **E18 records at the point of decision.** The declaring table of an edge is
  a field of `DependencySpec` set by the parser; the adoption of E5 and the
  link-form reason of E4 are recorded where they are decided and read by the
  writer. No reader reconstructs a decision from output text.

### 1.2 Stability

- **E8, Mach-O names outside the loader's reach.** A dependency dylib whose
  install name is an absolute path outside `/usr/lib/` and `/System/Library/`
  resolves to a file, but a staged copy beside the program is not what dyld
  loads. Revision 2 would have staged it and written `walked`. Revision 3: a
  Mach-O name is a closure member only when it is `@rpath/<file>` and the
  loading image carries an rpath of exactly `@loader_path` or
  `@executable_path`, or when it is `@loader_path/<file>` or
  `@executable_path/<file>`; any other non-platform name makes the closure
  `not-walked`, naming it. The tree then says what a machine without the build
  tree will do.
- **E12 normalises the driver's answer.** A driver answers
  `<root>/bin/../lib/<triple>/libc++.a`; the path is made lexically normal
  before it enters a link line, so the LLVM payload's host link line is
  byte-identical (the triage's negative criterion). A driver that does not
  know the file echoes the bare name; that answer is treated as a miss and the
  directory search runs.
- **E20 is measured before it is implemented.** Rewriting the registry's
  `.xlings.json` changes the file xlings reads, not necessarily the index
  checkout xlings already cloned. The task starts with a local reading: after
  the file names a checkout on an existing home, does `mcpp index update`
  re-point `data/xim-pkgindex`? The implementation follows the reading; if the
  checkout is not re-pointed, the engine names the stale checkout and the
  command that replaces it, and never deletes it silently.

### 1.3 Simplicity

- **E13, one file format with a separator paths do not contain in practice.**
  Revision 2 wrote `<destination> <absolute source>` separated by a space; a
  Windows user directory commonly contains one. Revision 3: one line per file,
  destination and source separated by a TAB, destination relative to the
  artifact's directory with `/` separators. The file exists for every runner
  invocation, empty when there is nothing to transfer, so a runner can tell an
  engine that states "nothing" from one that predates the variable.
- **E17, measure before adding a mechanism.** `mcpp::runner(name, token)`
  appends one token per call (`hostprogram.cppm:78`), and the tokens are the
  argv by contract (`directives.cppm:866-878`); two modules in one build
  program cannot be told apart in the directive stream. Adding attribution to
  the stream is a larger change than the defect. The task first measures the
  case that matters to B3: a manifest's `[target.<t>.runners] <name>` and a
  graph-supplied runner of the same name. If the two argvs are concatenated,
  the manifest's declaration wins and the graph's is reported, which is the
  precedence every other manifest-versus-graph conflict follows; the
  in-program case is stated in docs/30 as the contract it is.

### 1.4 User experience

- Every new refusal and warning names the file, the key as written, and the
  form that works (E2, E4, E5, E6, E9, E16, E19, P1). A warning is written
  once per build, not once per edge.
- The graph record (E18) is the single place a user reads why a package has
  its identity, its declaring table and its link form; `mcpp why deps` prints
  it in the same order `resolution.json` stores it.

### 1.5 Compatibility and upgrade without notice

| change | an existing manifest or package | an engine one release older, given a manifest written for this one |
|---|---|---|
| E1 | no manifest among 509 declares one key in both tables; builds unchanged | ignores the conditional declaration, as today |
| E3 | packages without the sub-table unchanged | skips the sub-table without a word (the triage records this); the package states its floor in its documentation |
| E3 sweep | a sub-table the parser reads is never reported; the list is taken from the parser, and a unit test fails when a new reader is added without the list | not applicable |
| E5 | nine edges gain one warning and keep building | unchanged |
| E9 | an Android `dir`/`tar` that shipped an incomplete tree now carries its closure, or refuses naming what is missing; a Mach-O `dir` that refused now succeeds | stage manifests gain lines; no official plugin reads the stage manifest today (`grep` over mcpp-plugins at `9301832`) |
| E11 | every ELF shared library gains `DT_SONAME` equal to its file name; consumers' `DT_NEEDED` is unchanged because they link with `-l<name>` | not applicable |
| E12 | host LLVM link lines byte-identical; Android test programs become self-contained | not applicable |
| E13 | runners that ignore the variable are unchanged | the variable is absent; `adb-run` then transfers nothing, as today |
| E16 | a plain `mcpp run` is unchanged; `--format <f>` without a runner named `<f>` is unchanged except that a directory is refused before the spawn instead of failing in it | not applicable |
| E18 | every existing `resolution.json` field unchanged; `schema_version` follows docs/50 §7's rule for additive fields | not applicable |
| E19 | the refusal's wording changes; no e2e test asserts the old sentence (`grep "this machine has" tests/e2e`: none in a floor test) | not applicable |
| E20 | a home whose `config.toml` carries an override that was silently ignored now takes it, and the reconciliation prints one line naming the index and both URLs | not applicable |
| P1 | a stage without `needs` lines comes from an engine below the floor; the member refuses naming the floor instead of packing an APK without the dependency's library | not applicable |

### 1.6 Cross-platform

| task | Linux x86_64 (local) | Linux CI | macOS CI | Windows CI | device |
|---|---|---|---|---|---|
| E1-E7, E18-E21 | e2e | e2e shards | e2e | e2e | - |
| E8-E9 Android | e2e (`android-ndk`) | E22 job | - | - | E22 emulator |
| E8-E9 Mach-O | - | - | e2e 666 and a new test | - | - |
| E8 PE | existing e2e | - | - | e2e | - |
| E10 | unit test for all four tokens; e2e with `$ORIGIN` and `@executable_path` spellings | e2e | e2e reads `LC_RPATH` | - | - |
| E11 | e2e (`readelf`) | e2e | - | - | - |
| E12-E13 | e2e (`android-ndk`: `DT_NEEDED`; a fake runner reads the file) | E22 | e2e (fake runner) | e2e (fake runner) | E22 emulator |
| E16 | e2e (`--format dir` refusal; fake runner) | e2e | e2e | e2e | - |
| P1, P5 | local | plugins CI | - | - | plugins CI emulator |
| P2-P4, P7 | - | - | plugins CI | - | iOS simulator |
| P6, X4 | - | - | - | plugins and xim-pkgindex CI | - |

A cell marked `-` is a row the task does not change.

### 1.7 Consistency

- The new keys use existing vocabularies: `kind` is `[targets.<n>] kind`,
  `discover` takes `[build] sources` globs, the facts are `version-floor`
  facts, the accessor joins the `toolchain_*()` family, `--toolchain` is
  `build --toolchain`.
- Every new record line follows its file's existing grammar: `needs` is one
  more line kind in `.stage-manifest`, `graph` is one more top-level member of
  `resolution.json`.

### 1.8 Test coverage, and where a test would stay green over a regression

- **Every criterion is run once with the fix removed.** For each engine task
  the test is run against the released 2026.9.14.1 and must fail there; the
  reading is recorded in the pull request. A test that passes on both engines
  measures nothing.
- **The Android e2e tests never ran in CI.** `run_all.sh` detects
  `android-ndk` from a payload path, and no workflow installs the payload, so
  e2e 652b and 664 are local-only. E22 adds the job that installs the NDK and
  runs them, plus the new Android tests, and one emulator step for E12/E13.
- **The criteria read state.** A1, A2 and the per-row kind assert on the
  `graph` record (E18); a warning's sentence is asserted only where the
  sentence is the deliverable (E2, E4, E5, E19).
- **Negative directions** are the triage record's, per task, and are tests,
  not prose.

### 1.9 Refinements found while implementing

Each item was found by a measurement on the branch, and each has a test.

- **A2 is keyed by source, not by key.** A map from a key an edge wrote to the
  identity its manifest declared would also have captured a `version`
  dependency written with the same key, which is a different package. The
  resolver records the identity resolved from each canonical source (a
  directory, or a repository and reference); a second key over that source
  takes it, and a manifest that declares no namespace makes the second key a
  refusal (e2e 679).
- **The `[target.<sel>]` sweep reports sub-tables.** The sweep skipped every
  table-valued key because a hand-written list of sections had drifted
  twice. The list now exists beside the scalar lists and a unit test derives
  the parsed sections from the parser's source in both directions, so the
  drift is a failing test rather than a false warning.
- **A row's `kind` is checked after target inference.** A library target a
  package does not declare exists only after `load` infers it, so the name
  is validated there rather than where the row is parsed.
- **The stage manifest's `needs` lines are TAB-separated and carry a third
  value.** `needs<TAB><name><TAB><staged path|platform|unresolved>`: a Mach-O
  install name or a Windows directory can contain a space, and a provider
  reads a failure as state. The ELF host row applies §5.3.4 too: a name the
  loader reports as not found makes the closure `not-walked`.
- **The Android search set includes `[runtime] link_library_dirs` and the
  transitive needed directories.** Without them a valid prebuilt library was
  refused (measured).
- **The default SONAME precedes `$ldflags`.** A project that names its SONAME
  through `[build] ldflags` keeps it, because the linker takes the last
  `-soname` (e2e 220).
- **The NDK is asked with the API level, and its `libc++.a` is a linker
  script.** Without the level the NDK answers with the host's archive. With
  the script's archive names passed to `--exclude-libs`, a self-contained
  Android shared library exports 4 dynamic symbols instead of 161.
- **`MCPP_RUNTIME_FILES` lists the linked shared libraries too.** A test on a
  row whose dependency is shared (A1's per-row kind) needs the library beside
  it on the device; a nested test's destinations start with `../`.
- **A directory distributable that meets no runner exits 126.** The
  exit-code specification forbids moving a published failure to another
  category, and the kernel's refusal of the same case was 126 (docs/50 §6).
- **§9 item 8 (E17).** Measured on 2026.9.14.1: a manifest runner and a
  build-program runner of one name do not concatenate (the manifest wins);
  two emissions in one build program form one argv by the directive contract,
  now stated in docs/30; a runner a dependency's build program supplies and
  the root's build program also emits became one argv (`run-A.sh run-B.sh
  <artifact>`). The last is refused naming both, unless the manifest declares
  the name, which is the precedence every other runner conflict follows
  (e2e 684).
- **A test built from a subdirectory could not load a graph-built shared
  library.** Its search path named its own directory (`bin/sub/`), and the
  post-link closure check refused the build on 2026.9.14.1. A consumer outside
  the library's directory also searches the relative path to it (e2e 685).
- **A refused `--toolchain` value was credited to `[toolchain].<platform>`.**
  The message names where the value was written (e2e 671).
- **C4, measured before implementing.** On an existing home, once the
  registry's `.xlings.json` names a checkout, `mcpp index update` re-points
  `data/xim-pkgindex` at it. Removing the table restores the previous entry,
  through a record of what mcpp wrote (`.mcpp-index-overrides.json`), unless
  the entry was changed after mcpp wrote it (e2e 681).
- **C4 reconciles tables only (found by the independent review of the pull
  request).** The reconciliation compared every index repository the
  configuration resolved, including the `mcpplibs` entry mcpp adds when
  `config.toml` has no table for it, so a registry copy of that entry which
  differed from the default was rewritten with a line naming a table that
  did not exist, and a later `[index.repos.mcpplibs]` table could not be
  undone by removing it. The review's premise was partly wrong: a home mcpp
  creates writes the `mcpplibs` table into its own `config.toml`, where the
  line is accurate; the defect needs that table removed. Only a table is now
  reconciled, and a name counts as configured only while a table names it
  (e2e 681 E and F, which fail without the change).

## 2. Repositories, branches, versions

| repository | branch | version | merges after |
|---|---|---|---|
| mcpp-community/mcpp | `feat/634-cmake-parity` | 2026.9.14.2, or the first free number of the release date | its CI |
| openxlings/xim-pkgindex | `feat/634-runners-and-payloads` | `macapp-run` 0.1.0; `apple-simulator-tools` 0.3.0; `android-platform-tools` next revision; `wix` 5.0.2 with the extension; `bundletool` upstream's current release; `libxml2` a revision that re-runs the install hook | its CI; independent of the engine release |
| mcpp-community/mcpp-plugins | `feat/634-closure-bundles` | 0.10.0 | R1 (its CI pins the released engine) |
| mcpplibs/mcpp-index | `feat/634-plugins-0.10.0` | - | R2 |
| mcpp-community/mcpp (pin) | `ci/bootstrap-pin-<version>` | version group 2 | R1 and the bot's index pull request |

Why the plugins pull request merges after the engine release: P1 and P2 read
the stage E9 writes, P3 needs E16, and its CI must measure the released engine
rather than a branch. During development its CI is pointed at the engine
branch through a temporary `MCPP_SOURCE_REF` channel on every step that
installs mcpp, removed before merge.

## 3. Engine tasks in detail

Paths are relative to the mcpp repository. "Test" names the e2e test added or
changed; numbers are assigned per work tree (W1: 667-670, W2: 671-676, lead:
677-690) so that parallel branches do not collide.

### E1-E4 (A1)

- *Code.* `merge_conditional_config` (`src/build/prepare.cppm:367-472`): for
  each conditional map, erase unconditional entries of the same identity,
  then insert. `ConditionalConfig` (`modules/manifest/src/types.cppm:1276`)
  gains the per-row target kinds; `DependencySpec` gains the declaring table.
  The parser (`modules/manifest/src/toml.cppm`): the conditional dependency
  label (`:3015-3024`), the option-named selector warning in
  `load_selector_dep_table` (`:1599-1629`), the `targets` sub-table, and the
  sweep's list (`:2748-2760`). The degradation sentence in the linkage pass
  (`prepare.cppm:11280-11375`).
- *Tests.* Host row, both directions: the conditional `linkage = "shared"`
  gives `NEEDED libfw.so` on the matching row and a static link on a
  non-matching one; the modifier-only table exits non-zero naming the
  conditional section and the restated form; a per-row `kind` gives
  `bin/libfw.so` on Linux and a static link under a selector that does not
  match; `linkage = "static"` on the root's edge warns naming the selector and
  `--strict` exits non-zero. e2e 86, 195, 328 unchanged.

### E5-E7 (A2)

- *Code.* The resolve loop (`prepare.cppm:6499+`, dedup `:6531-6534`, name
  check `:7242-7260`, namespace fill `:7267-7270`); the scanner's
  duplicate-provider message.
- *Tests.* Two edges keyed `fw` and `huxdemo.fw` over one directory build,
  compile the unit once, warn once naming the requester, `fw`, `mcpplibs.fw`
  and `huxdemo.fw`; `graph` shows one package with both keys. Two keys `a.fw`
  and `b.fw` over a manifest without a namespace are refused before scanning.
  Matching keys warn nothing. `examples/04`, `08`, `12` build without the
  warning.

### E8-E11 (A3, A4, §9 item 11): work tree W1

- *Code.* `src/pack/pack.cppm` (`pe_closure` `:999-1053` generalised;
  `run_shared_program` `:1281-1337`; the Mach-O branch `:1423-1438`; search
  directories `:497`; `ClosureResult` `:247`), `src/pack/stage_tree.cppm`
  (`:126`), `src/pack/pipeline.cppm` (`:438-478`), the binary readers;
  `normalize_ldflag` (`src/build/flags.cppm:266-283`) and its copy
  (`prepare.cppm:6141-6158`) call one shared predicate;
  `shared_soname_flag` (`src/build/ninja_backend.cppm:274-290`), with the
  declared-soname alias logic unchanged.
- *Tests.* Android (`# requires: elf gcc android-ndk`): the `dir` tree holds
  `lib/libapp.so`, `lib/libfw.so`, `lib/libc++_shared.so`, the manifest says
  `walked` and lists `libc.so`, `libm.so`, `libdl.so` as platform; a needed
  library deleted after the build makes `dir` refuse naming it; two triples
  stage `lib/<abi>/` each with its closure. macOS: e2e 666's `dir` direction
  changes to the staged closure; the staged program runs after the build tree
  is moved and fails with "Library not loaded" when the staged dylib is
  removed. ELF host: the tree is byte-identical to 2026.9.14.1's and the
  manifest differs by `needs` lines. SONAME: `readelf -d` shows `SONAME
  libfw.so` on the host and Android rows and on the application object; a
  declared `soname` keeps its value and alias. rpath: a unit test over
  `$ORIGIN`, `${ORIGIN}`, `@executable_path/..`, `@loader_path`, `@rpath/x`, a
  relative path and an absolute path; an e2e shows the literal token in the
  program's rpath.

### E12-E17 (A6, A10, A5, B3, §9 item 8): work tree W2

- *Code.* `find_archive` (`src/build/flags.cppm:1064-1079`, `1182-1183`) asks
  the driver through the toolchain module (`src/toolchain/clang.cppm:130-150`
  is the existing query) with the effective target flag, cached with the
  toolchain probe; the degradation (`src/build/distribution.cppm:718-726`).
  Runner spawns in `src/build/execute.cppm` (`:643-711`, `:1602`, `:1691`,
  `:1873-1904`, `:2423`) write the runtime-files list from the plan's
  `runtimeDeployFiles` and set the variable. `src/cli.cppm` declares
  `--toolchain` on `run` (`:391`), `test` (`:480`), `pack` (`:574`).
  `src/build/test_targets.cppm:38` takes `[test] discover`, parsed by a new
  reader in the manifest module. The named-runner lookup for `--format`.
- *Tests.* Android row without `ldflags`: test programs name no
  `libc++_shared.so`; the host LLVM link line is unchanged. A fake runner
  script prints the variable's file; `mcpp test` and `mcpp run` hand it a file
  whose lines are `data/data.txt<TAB><abs>`; an artifact with no deployed
  files gets an empty file. `mcpp test --toolchain llvm@22.1.8` compiles with
  clang when the default is gcc. `discover = ["checks/**/*.cpp"]` runs
  `checks/a.cpp` and not a failing `tests/b.cpp`; `discover = []` runs none;
  without the key the names equal today's. `run --format <f>` with a manifest
  runner named `<f>` runs it; a directory distributable without one is refused
  before the spawn naming the runner name.

### E18-E21

- *E18 code.* The `resolution.json` writer (`prepare.cppm:12700-12870`),
  `mcpp why deps` (`src/doctor.cppm:1089-1110`). *Test.* The A1, A2 and
  per-row kind fixtures read `graph`; the existing fields are unchanged.
- *E19 code.* `min_platform_version` (`prepare.cppm:1619-1670`),
  `checkVersionFloors` (`:7804-7849`). *Test.* Android row: a dependency
  requiring `android.api-level >= 23` is refused for a root stating 21 and for
  a root stating nothing, naming `min_api_level`; 24 builds; the host row is
  silent.
- *E20 code.* `src/config.cppm:535-549`, `:588-596`, the provisioning line.
  *Test.* An existing home whose `config.toml` gains `[index.repos.xim]`
  pointing at a local index checkout with an extra recipe installs it on the
  next build and prints the reconciliation line once.
- *E21 code.* `src/build/hostprogram.cppm` (the accessor and its contract
  entry), the environment the build program receives. *Test.* A build program
  prints the accessor; the value names `<registry>/subos/default/usr/lib/pkgconfig`.

### E22 (CI)

A job in `ci-linux-e2e.yml` installs `xim:android-ndk` into the job's home
with a cache keyed by the payload version, runs the e2e tests that require
`android-ndk`, then starts an API 34 x86_64 emulator
(`reactivecircus/android-emulator-runner@v2`, KVM enabled, the configuration
measured on mcpp#635) and runs `mcpp test` on a fixture whose test programs
read a deployed file, through `adb-run`. The emulator step is marked with the
xim-pkgindex revision it needs; until X3 is published it asserts only the
program's loading (`runs`), and the file criterion is added when the index
carries X3.

### E23 (documentation)

| document (and `docs/zh/` mirror) | changes | owner |
|---|---|---|
| `docs/04-mcpp-toml.md` | conditional dependency replacement; `[target.<sel>.targets.<n>] kind`; `[test] discover` | lead, W2 |
| `docs/05-dependencies.md` | identity adoption for `path`/`git`; the graph record's link-form reasons | lead |
| `docs/08-testing.md` | `discover`; test programs on device rows; `--toolchain` | W2 |
| `docs/09-commands-by-scenario.md` | `--toolchain` on `run`/`test`/`pack`; `why deps` | W2, lead |
| `docs/10-pack-and-release.md`, `docs/12-binary-distribution.md` | the Android and Mach-O closure; `needs` lines; `not-walked` | W1 |
| `docs/22-target-side.md` | default SONAME, rpath loader tokens | W1 |
| `docs/23-the-project-environment.md` | `config.toml` index overrides on an existing home | lead |
| `docs/30-build-mcpp.md` | `pkg_config_libdir()`; platform facts; runner emission contract | lead, W2 |
| `docs/41-devices.md` | `MCPP_RUNTIME_FILES`; format-named runners | W2 |
| `docs/50-machine-output.md` | `graph`; `needs` | lead, W1 |
| `docs/specs/manifest-semantics.md`, `package-identity.md` | replacement; adoption | lead |

The prose follows `.agents/skills/mcpp-docs-style`: usage for implemented
behaviour, declarative, no design reasoning, bilingual parity.

## 4. Payload, plugin and index tasks in detail

- **X1** `pkgs/m/macapp-run.lua` (macOS): the program requires
  `Contents/Info.plist` (exit 2 naming the missing piece), reads
  `CFBundleExecutable` with `plutil`, and `exec`s `Contents/MacOS/<exe>` with
  the arguments. CI: a bundle whose program prints its arguments and exits 7
  returns 7 with the output.
- **X2** `pkgs/a/apple-simulator-tools.lua` 0.3.0: after `simctl install`,
  `otool -L` on the executable decides; without UIKit, `simctl spawn <device>
  <container>/<exe> <args>`; with UIKit, `simctl launch --console-pty` and one
  line saying the status is simctl's. CI: exit 7 and abort 134 are returned
  for a non-UIKit bundle, twenty runs each keep the marker.
- **X3** `pkgs/a/android-platform-tools.lua`: when `MCPP_RUNTIME_FILES` names
  a file, every line's source is pushed to `<remote dir>/<destination>` and
  the program runs with that directory as its working directory. CI on the
  emulator: a program reading `data/data.txt` relative to its directory exits
  0.
- **X4** `pkgs/w/wix.lua`: `mbanative.dll` anchor, the `.wixext` package,
  a Windows job that installs the payload and checks the anchors, and the ABI
  statement in the recipe's notes.
- **X5** `pkgs/b/bundletool.lua`: the upstream jar and a launcher through the
  JDK payload `dist-apk` already uses.
- **X6** `pkgs/l/libxml2.lua`: relocate and declare `libxml-2.0.pc` as
  `gtk4.lua` does; a revision so an installed copy re-runs the hook.
- **X7** The pull-request workflow creates a fresh home whose `config.toml`
  names the pull request's checkout and builds a consumer fixture that
  declares a recipe the pull request adds or changes.
- **P1-P9** as §0.3; each has a job in the plugins CI, and P1's criteria are
  two packs in a row carrying the dependency's library, two triples giving one
  APK listing both ABIs, and a refusal whose reason is printed.
- **I1** `pkgs/m/mcpp.plugins.lua`: a 0.10.0 entry in the bot's shape (one
  `latest` line replaced), and a note stating the engine floor 2026.9.14.2 and
  what an older engine does (P1's refusal).

## 5. Order

```
X1..X7 ─────────────────────────────► R3 ──┐
E1..E7, E19..E21 (lead) ─┐                  │
E8..E11 (W1) ────────────┼─► E18 ─► E22 ─► E23 ─► E24 ─► R1 ─┬─► V1 ─┐
E12..E17 (W2) ───────────┘                                   │       │
P1..P7 (W4, against the engine branch) ──────────────► P8 ◄──┘       │
                                                        P9 ─► R2 ─► I1 ─► R4 ─► V2 ─► V3 ─► V4
```

The engine pull request merges only with every CI workflow green on its last
commit and the triage record's revision 3 folded in. The release follows
`.agents/skills/mcpp-release`: version group 1 in the pull request, tag,
`release.yml`, `publish-ecosystem`, the bot's index pull request merged with
`--admin`, `latest` read back, GitCode assets compared, version group 2 in a
separate pull request.

## 6. Verification

### 6.1 In CI

Every pull request's CI is green on its last commit, read from the run of that
commit's SHA. For the engine, each new e2e test also has a recorded failing
run against 2026.9.14.1 (§1.8).

### 6.2 In a sandbox, after release

A SubOS created with `xlings subos new m634`, entered with `xlings subos use
m634 --sandbox --cmd "..."`, with `xlings config --mirror CN` and `mcpp self
config --mirror CN` set inside it. Each check reads the installed version
first.

| id | check | criterion |
|---|---|---|
| S1 | `mcpp --version` | the released version |
| S2 | A1 fixture on the host row | `NEEDED libfw.so`; `graph` names the conditional table |
| S3 | A2 fixture | one warning; `graph` one package, two keys |
| S4 | A4 | `SONAME libfw.so` |
| S5 | A5 | `discover` runs `checks/a.cpp` only |
| S6 | A10 | `mcpp test --toolchain llvm@22.1.8` compiles with clang |
| S7 | X | `mcpp why deps` prints the graph for a path dependency |
| S8 | A3 and A6 on `x86_64-linux-android` | `dir` tree with `libfw.so` and `libc++_shared.so`, `walked`; test programs name no `libc++_shared.so` |
| S9 | A9 | the floor refusal names `min_api_level` |
| S10 | A7 | `pkg-config --cflags --libs gtk4` with the accessor's value exits 0 |
| S11 | C4 | an existing home takes a `config.toml` override on the next build |
| S12 | B3 | `mcpp run --format dir` is refused before the spawn naming the runner |
| S13 | P1 through the index | two packs carry the dependency's library; two triples give one APK with both ABIs |
| S14 | I1 | `mcpp.plugins` resolves to 0.10.0 |
| S15 | X6 | the view holds `libxml-2.0.pc` after installing `xim:libxml2` |

## 7. The ecosystem review (V3)

After V1 and V2: every ledger row is `done` or `dropped` with a reading or a
reason; the four repositories' `main` heads build green; the index's `latest`
entries point at what was released; the documentation describes what the
sandbox measured; the triage record and this record are marked landed; the
reply on #634 lists, per item, what landed, where, and what the project can
now remove.

## 8. Parallel work trees

| tree | path | branch | owns |
|---|---|---|---|
| lead | `../mcpp-634` | `feat/634-cmake-parity` | everything not listed below; integration |
| W1 | `../mcpp-634-pack` | `feat/634-pack` | `src/pack/**`; `normalize_ldflag` and its copy; `shared_soname_flag`; e2e 667-670; the unit test for the rpath predicate; docs 10, 12, 22 |
| W2 | `../mcpp-634-run` | `feat/634-run` | `find_archive` and the driver query; `src/build/execute.cppm`; `src/build/test_targets.cppm`; `src/cli.cppm` option declarations; the `[test]` reader, inserted as a new function; `modules/buildmcpp/src/directives.cppm`; e2e 671-676; docs 08, 41, the `--toolchain` lines of 09 |
| W3 | xim-pkgindex work tree | `feat/634-runners-and-payloads` | X1-X7 |
| W4 | mcpp-plugins work tree | `feat/634-closure-bundles` | P1-P8 |

Each engine work tree builds its own mcpp and runs only its own tests and the
e2e tests that cover the files it changes; a built binary is copied to a fixed
path before a suite runs against it. The lead merges W1 and W2 into the
feature branch, resolves conflicts, and runs the full suite once on the
merged tree. No work tree kills processes by pattern.
