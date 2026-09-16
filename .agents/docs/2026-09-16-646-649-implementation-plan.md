---
subject: triage
status: landed
---

# #646 to #649 implemented: the plan, its review from eight angles, and the ledger

**Status:** closed on 2026-09-16. Every row of §0 is closed with a reading, and
the ecosystem is verified end to end: a SubOS sandbox with CN mirrors runs the
scenarios against the published engine, plugins and index and reads
`version=2026.9.16.1 fails=0` (§9.2), where the same script against 2026.9.15.2
reads `fails=10`.

mcpp-community/mcpp#650 (`f4529b2a`) carried the engine work with 40 of 40
checks green. 2026.9.16.1 ships from #651 (`2f925d48`) rather than from that
commit: the review before the release found that #650 refuses a manifest
2026.9.15.2 builds, and the dispatched release was cancelled to carry the fix
(§1.10 item 19). This record turns the decisions of
`2026-09-16-646-649-four-issues-by-home.md` ("the triage record") into work:
one pull request per repository, the order in which they merge and release,
the criterion each task is held to, and a ledger whose rows close only with a
reading. The triage record's decisions D1 to D10 are adopted; §1 states the
refinements this review makes before code is written, and a refinement found
while implementing is appended to §1.9.

Base commits: mcpp `2fc7b5b0` (2026.9.15.2), mcpp-plugins `d6bee6a` (0.11.1),
mcpplibs/mcpp-index, openxlings/xlings `3cd8061`, openxlings/xim-pkgindex at
their heads on 2026-09-16. The engine release is 2026.9.16.1; the plugins
release is 0.12.0.

## 0. The ledger

Status is `todo`, `doing`, `branch` (implemented on the pull request's branch,
with its reading), `done` (merged or published, with its reading) or `dropped`
(with the reason). Owner `lead` is the integrating session; `W1` to `W3` are
the parallel work trees of §8. Triage sections are cited as `T§`.

### 0.1 Engine: mcpp-community/mcpp, one pull request (`feat/646-649`)

| id | task (triage §) | owner | depends on | status |
|---|---|---|---|---|
| L1 | the refresh decision walks the resolver's deprecated bare-name rung before calling a miss (T§7.0) | lead | - | done (#650): unit `PmIndexRefresh.BareNameResolvedThroughTheLegacyRungIsNotAMiss`; e2e 730 (fails on 2026.9.15.2: the decision asked for a refresh of `cjson@1.7.19`) |
| L2 | the saved standard output is close-on-exec, and not inheritable on Windows (T§7.2) | lead | - | done (#650): e2e 731 (fails on 2026.9.15.2: a build program holds the caller's pipe as descriptor 3) |
| L3 | xlings runs under the owned launcher; a total deadline for the index refresh, an inactivity deadline for installs (T§7.3) | lead | L2 | done (#650): e2e 732 (fails on 2026.9.15.2: `index update` returned after 60 s under a 3 s bound); SIGTERM to mcpp took the stub's group (local) |
| L4 | a per-run record of observed effects; `network` when a network child is launched (T§7.4) | lead | L3 | done (#650): e2e 733 (fails on 2026.9.15.2: a plan that refreshed the index reports no `network`) |
| L5 | the three refreshes that bypass `mcpp.pm.index_refresh` go through `decide_for_miss` (T§7.5) | lead | L1 | done (#650): e2e 734 (fails on 2026.9.15.2: the unsynced custom index was synced under `auto_refresh = false`) |
| L6 | the refusal code `offline-download-required` at every offline refusal site; `MCPP_OFFLINE_DOWNLOAD_REQUIRED` in the envelope (T§7.1) | lead | - | done (#650): e2e 733 leg B and 735 (fail on 2026.9.15.2: `MCPP_BUILD_DATABASE_PLAN_FAILED`) |
| L7 | the default `artifact` of the mcpplibs index is the region object; the existing `.xlings.json` migration carries it (T§7.6 step 2) | lead | X1 | done (#650): e2e 151 (fails on 2026.9.15.2: no region artifact); traced `mcpp index update` with `mirror = CN` connects only to raw.gitcode.com, gitcode.com and file-cdn.gitcode.com |
| R1 | ELF: programs and tests over a plan-built C++ shared library take the shared-library runtime contract; an explicit `self-contained` program there is refused (T§4.1, D1) | W1 | - | done (#650): unit `Distribution.*`; e2e 700 (fails on 2026.9.15.2: the default llvm shape aborts with `std::bad_cast`) |
| R2 | symbol provision: `STB_GNU_UNIQUE` is vague linkage; a duplicate whose definitions come from one plan object is not reported (T§4.2) | W1 | - | done (#650): unit `SymbolProvision.*`; e2e 701 (fails on 2026.9.15.2: `--strict` exits 1) |
| R3 | a static package reachable from one shared image only is linked into that image; one reachable from two images is refused where the link or load cannot succeed and diagnosed elsewhere (T§4.3, D2, §1.5) | W1 | - | done (#650): unit `StaticPlacement.*` (9); e2e 702 and the rewritten 307 (fail on 2026.9.15.2: `undefined symbol: x_answer` through a foreign `dlopen`) |
| R4 | clang on the MSVC ABI records the runtime it delivers; an undeliverable `cxx_runtime` is diagnosed; docs/20 states the row's model (T§4.5 step 1) | W1 | - | done (#650): e2e 703 (Windows CI) |
| R5 | measurement legs: exception identity across a Mach-O dylib under the payload default (macos-15), and across an llvm-row DLL (windows-2022), each printing its reading (T§4.4, §9.2) | W1 | - | done: macos-15 run 35032727668 reads `macho default: runtime_error=not-matched own_error=caught errc=unequal`, `macho host-coupled: runtime_error=caught errc=equal`, `macho shared-host-coupled: not-matched`. F2 is confirmed; this release warns (`build/cxx-runtime-identity`) and leaves the default to its own record |
| G1 | the forward validator accepts a key declared in any dependency table on any row (T§6.1, X8) | W2 | - | done (#650): e2e 710 (fails on 2026.9.15.2: the two-level build-dependency forward refused under `--strict`) |
| G2 | a `[feature-deps]` restatement whose source differs is refused; docs/05 says to restate the source (T§6.2, D8) | W2 | - | done (#650): e2e 711 (fails on 2026.9.15.2: the differing restatement is not refused) |
| G3 | one helper names a provider for a consumer; `dep_bin` gains the qualified spelling (T§6.3) | W2 | - | done (#650): e2e 711 and a new 187 leg (fail on 2026.9.15.2: `dep_bin("spike.installer")` reads nothing) |
| G4 | a package with no library target contributes nothing to a consumer's target graph; package cycles are checked at resolution; a repeated tool is refused at its first repetition (T§6.4, D3, X2, X3) | W2 | - | done (#650): e2e 712 (fails on 2026.9.15.2: the feature tool depending on its declaring package does not build); scan of 902 manifests found no consumer relying on the old edge |
| G5 | a git dependency selects a repository member by identity; a second declaration merges additively; the git banner names the commit (T§6.5, D9, X1, X6) | W2 | G4 | done (#650): e2e 713 (fails on 2026.9.15.2: a git dependency naming a repository member is refused) |
| G6 | `--features dep/feature` is a root forward; `why deps --features` (T§6.6, X7) | W2 | G1 | done (#650): e2e 714 (fails on 2026.9.15.2: `--features spike.fw/installer` does not open the feature) |
| S1 | `mcpp::graph_file()` for the root build program, with `[package.metadata]` and a digest in the re-run key (T§5.1, D4) | W3 | - | done (#650): e2e 720 (fails on 2026.9.15.2: `graph_file` is not a member of `mcpp`) |
| S2 | the link branch is chosen by host and target object format, as a pure function with a host-by-row unit test (T§5.3) | W3 | - | done (#650): unit `LinkShape.*`; e2e 721 (macOS CI step) |
| S3 | pack strips the program on every stripping row, every graph-built shared library and staged runtime copies; the status line reports what was done; `mcpp::pack_strip()` and `mcpp::pack_debug_symbols_dir()` (T§5.4, D5) | W3 | - | done (#650): e2e 722 (fails on 2026.9.15.2: `lib/libdep.so is not stripped: symtab=1 debug=7`; the Android leg ran locally) |
| S4 | `mcpp pack --message-format json` prints one `mcpp.pack` envelope; `pack --release/--dev`; `run` takes `build`'s profile precedence (T§5.5, D6, X5) | W3 | - | done (#650): unit `BuildProfile.*`; e2e 723 (fails on 2026.9.15.2: `unknown option: --message-format`) |
| S5 | `[package]` warns about an unknown key, `metadata` included in its known set (X4) | W3 | S1 | done (#650): unit `Manifest.PackageMetadata*`, `UnknownPackageKeyIsReported` |
| C1 | CI: new llvm-dependent e2e scripts run on the hermetic job with their PASS lines asserted; the macOS and Windows measurement legs print their readings to the job summary | lead | R1-R5, S2 | done (#650): 700 on the hermetic job; READING lines to the macOS and Windows summaries; 721 as its own macOS step |
| C2 | user documentation with its Chinese mirror, docs/50 codes and kinds, CHANGELOG, version 2026.9.16.1 | lead, W1-W3 | all | done (#650): docs 04, 05, 06, 07, 10, 20, 30, 50 with mirrors; SPEC-005 v1.1; CHANGELOG; 2026.9.16.1 |
| C3 | the two records closed with their readings | lead | all | done: the triage record is `status: closed`, and §9 below is this record's closure |

### 0.2 Plugins: mcpp-community/mcpp-plugins, one pull request (`feat/646-649`, 0.12.0)

| id | task | owner | depends on | status |
|---|---|---|---|---|
| P1 | `dist-apple` `options::omit_keys` over the defaulted keys (T§8) | lead | - | done (#28, 0.12.0): plan checks on Linux; the bundle on macos-15 CI |
| P2 | `dist-web` `options::page`, default `index.html` (T§8) | lead | - | done (#28, 0.12.0): `check-web-plan.sh` with a named-page leg |
| P3 | `dist-apk` follows `mcpp::pack_strip()` and `mcpp::pack_debug_symbols_dir()` when the engine provides them, and keeps its own strip otherwise | lead | S3 released | branch: `ad7a3dd`; unset and simulated variables verified; legs (m) and (n) ran in CI once the pin moved to 2026.9.16.1: `--no-strip packs the library with its symbol table` and `--debug-symbols: the packed library is the engine's stripped copy` |
| P4 | `rules-swift`: one package's Swift sources compile through an `object` action, a generated header through a `source` action, and the runtime link flags through `link_flag`; a macos-15 consumer runs (T§5.2) | lead | - | done (#28, 0.12.0): `all-rules-compile` on Linux; build, run and the header on macos-15 CI |
| P5 | `dist-apk` and `dist-apple` collect library contributions from `[package.metadata.dist-apk]` and `[package.metadata.dist-apple]` through `mcpp::graph_file()` when present | lead | S1 released | done (#28, 0.12.0): `tests/apk-consumer-graph` five legs, including the precedence between two contributors (§1.10 item 21); `tests/ios-app-consumer` two-dependency plist merge |

### 0.3 Index, payloads and projects

| id | task | owner | depends on | status |
|---|---|---|---|---|
| X1 | mcpplibs/mcpp-index: the artifact is byte-reproducible, and a version whose GitCode asset differs is republished under a new name (T§7.6 step 1) | lead | - | done: mcpplibs/mcpp-index#432 (`0cbac960`); the publish of `0cbac96` verified both forges itself, and the probe reads pointer, GitHub and GitCode all `02a437110017` (547679 bytes) |
| X2 | openxlings/xim-pkgindex: the release bot's `mcpp` bump merged | lead | release | done: openxlings/xim-pkgindex#845 (`1c951f0b`), 16 of 16 checks; the index artifact republished as `v1c951f0` and a consumer resolves `xim@artifact:1c951f0` |
| X3 | mcpplibs/mcpp-index: the plugins descriptor 0.12.0; the index CI's `MCPP_VERSION` moves to 2026.9.16.1 | lead | X2, P1-P5 | done: mcpplibs/mcpp-index#433 (`c176883`). The pull request's own CI selected no workspace member, so the pin move was held to a dispatched full sweep: 27 jobs, 0 failures, the workspace matrix building on linux, macos and windows under both the default and llvm toolchains |
| X4 | openxlings/xlings: `compat.ftxui` and `compat.gtest` spelled with their namespace (T§7.0) | lead | - | done: openxlings/xlings#597 (`4ea4eac9`), CI 9 of 9 |

### 0.4 Verification

| id | task | owner | depends on | status |
|---|---|---|---|---|
| V1 | a fresh SubOS sandbox with CN mirrors for xlings and mcpp runs §7's scenarios against the published engine, plugins and index | lead | X2, X3 | done: `version=2026.9.16.1 fails=0`, seventeen assertions (§9.2); the control against 2026.9.15.2 reads `fails=10` |
| V2 | replies on #646, #647, #648 and #649 naming the releases and what each project writes | lead | V1 | done: one comment on each issue, stating what was measured, where the measurement differs from the report, and the assertion of §9.2 that closes it |

## 1. The review

Each decision was held to eight questions. The answers either confirm the
triage record's decision or refine it; a refinement is stated where the
decision is.

### 1.1 Architecture

- Every engine change lands in the module that already owns the question:
  runtime contracts in `distribution.cppm`, placement in `make_plan`, the
  refresh policy in `mcpp.pm.index_refresh`, child ownership in
  `mcpp.platform.process`, forwards in the feature pass of `prepare`,
  build-program facts in `hostprogram.cppm`, pack policy in `src/pack`. No
  task adds a module.
- The two new facts a build program reads (`graph_file`, `pack_strip`) extend
  the accessor family that exists (`dep_dir`, `pack_stage_dir`), reach the
  program through the contract environment, and therefore take part in the
  re-run key by construction.
- The refusal codes added here (`offline-download-required`,
  `program-cxx-runtime-split`, `static-package-in-two-images`,
  `package-cycle`) join the existing sink; `mcpp why toolchain` and the
  envelope read them without a second classification.

### 1.2 Stability

- L3 changes how every xlings child is started. The streaming sink, the exit
  code and the NDJSON parsing stay as they are; only the launcher changes. The
  deadline paths are exercised by a stub xlings named through
  `[xlings] binary`, on Linux in CI and on Windows through the e2e shard.
- R3 moves objects between images. Its unit test enumerates the closures of
  six graph shapes (a chain, a diamond through one image, a diamond through two
  images, a root-owned image, an image over an image, a header-only package)
  before any link line changes.
- G4 removes edges from the consumer graph. Before it is implemented, the
  manifests of the e2e corpus, the examples, mcpp-index and mcpp-plugins are
  scanned for a dependency on a program-only package that is not requested for
  `tools` (§1.9 records the result).

### 1.3 Simplicity

- A2 is one call. A5 deletes a derivation. L1 adds one condition to a lookup
  that exists. G3 deletes a duplicated derivation. E3's fix is a pure function
  replacing an `if constexpr` choice.
- The deadline design uses the launcher's existing total deadline for the
  refresh; only the install path needs an inactivity bound, which is one more
  parameter of the same launcher, not a second launcher.
- D6 adds a kind to the envelope table rather than a new output mechanism.

### 1.4 User experience

- The status line of `pack` states what was done (S3); the envelope states
  what happened (L4, L6); a refusal names its remedy (R1, R3, G2, G4); a
  deadline names itself and the configuration key that sets it (L3).
- **Refinement U1 (L3).** A timed-out refresh prints one warning naming the
  deadline and `[index] refresh_timeout`, and the build continues from local
  data; it is never silent and never fatal on its own.
- **Refinement U2 (G6).** `--features dep/feature` is accepted by `build`,
  `run`, `test`, `pack` and `emit build-database` alike, because they share
  one overrides structure; the help text of each says so.

### 1.5 Compatibility and seamless upgrade

Each change that alters what a working build produces is listed with why an
upgrade is not a cliff.

| task | what changes for a working build | why it is not a cliff |
|---|---|---|
| R1 | a gcc program over a C++ shared library it builds gains `NEEDED libstdc++.so.6` | the process already loads that file through the library; `pack` already bundles it |
| R3 | on ELF, a static package reachable only through a dependency's shared image moves from the program into the image | the program never referred to it; its symbols resolved to the same single copy |
| R3 | a static package reachable from two images | **Refinement C1.** It is refused only where the build cannot work today (Mach-O and PE, whose links fail, and the Android `app` row, whose loader binds the image before the program). On other ELF rows the build proceeds as it does today, with a degraded diagnostic naming `linkage = "shared"`; `--strict` fails it. Refusing a build that runs today on Linux would be an upgrade cliff for no gain on that row. |
| R4 | the recorded contract of an llvm-row PE artifact changes from `host-coupled` to `self-contained` | the artifact is byte-identical; the record now matches it |
| G4 | a program-only package's dependencies leave the consumer's link | §1.2's scan decides whether any consumer relied on it |
| S3 | packed shared libraries lose their symbol tables | `--no-strip` restores them; debug sections go to `--debug-symbols` as the program's do |
| S4 | `mcpp run --profile dev --release` builds `dev` | the same line already builds `dev` under `mcpp build` |
| L5 | `auto_refresh = false` blocks the pre-install refresh | the documentation already states that behaviour |
| L7 | a CN machine fetches the mcpplibs index from GitCode | only after X1 makes the GitCode artifact match its pointer; a mismatch falls back to GitHub, which is today's route |

No manifest key is removed and no manifest that loads today stops loading.
The new manifest surface is `[package.metadata.*]` (already ignored by every
client) and one configuration key, `[index] refresh_timeout`, whose absence
means the default.

### 1.6 Cross-platform

| task | Linux | macOS | Windows |
|---|---|---|---|
| L2 | `F_DUPFD_CLOEXEC` | `F_DUPFD_CLOEXEC` | `HANDLE_FLAG_INHERIT` cleared |
| L3 | process group, signal guard | process group, signal guard | job object with kill-on-close |
| R1 | the rule | not applicable (Mach-O is R5's measurement) | not applicable |
| R3 | ELF: diagnosed; Android app row: refused | refused | refused |
| R4 | unit test of the record | - | e2e on the llvm row |
| R5 | - | e2e reading | e2e reading |
| S2 | unit test over host by row | e2e: Android row links | unit test |
| S3 | ELF, Android | Mach-O unchanged (not stripped by design) | PE, MinGW DWARF |

### 1.7 Consistency

- Profile precedence, feature tokens, dependency names and runtime contracts
  each get one derivation (R2 of the triage record). The review confirmed that
  no second copy of these remains after the tasks: `run`, `build`, `test` and
  `pack` read one profile resolver; manifest forwards and CLI forwards share
  the validator; `fillDepDirs` and the tool publication share the name helper.
- **Refinement K1 (S4).** docs/50 §3 gains the rule for a command whose
  `--format` names its product, and `test`'s existing `--message-format json`
  is cited as the precedent, so the two spellings are one convention.

### 1.8 Test coverage

- Every task has a criterion that fails on 2026.9.15.2: an e2e script whose
  first line of output names the release it was measured against, or a unit
  test of a pure function.
- e2e scripts that need llvm run on the hermetic job and their PASS lines are
  asserted (the #641 lesson: `# requires: llvm` alone never runs on a shard).
- Measurement legs (R5) print `READING` lines into the job summary and do not
  fail on the value they read; they fail only if the probe could not run.
- The sandbox verification (§7) runs its script once against 2026.9.15.2 first:
  the change-detecting scenarios must fail there and pass on 2026.9.16.1, and
  the regression guards must pass on both.

### 1.9 Refinements found while implementing

1. **L5 adds a module.** `mcpp.pm.index_refresh` reads descriptors through
   `mcpp.pm.index_route`, which imports the fetcher, so the fetcher could not
   ask it. The policy half (reasons, policy, `decide_for_miss`, `apply`, the
   one-sync guard) moved to `mcpp.pm.refresh_policy`, which
   `mcpp.pm.index_refresh` re-exports. §1.1's "no task adds a module" does not
   hold for this task, and no other derivation of the policy remains.
2. **L3 keeps the shell command.** The xlings invocations keep their command
   strings (quoting, environment prefix, stderr redirection, all of which carry
   earlier Windows lessons) and run them through `run_streaming_bounded`, which
   is the deadline launcher with a streaming sink, an inactivity bound and, on
   POSIX, an opt-in process group registered with the signal guard. The group
   is opt-in because the launcher's uncaptured callers hand the terminal to
   their child, and a background group reading the terminal is stopped by
   SIGTTIN. A direct `xlings install -y` sends its output to the null device,
   so it has a total bound of three hours rather than an inactivity bound.
3. **X1 reuses rather than renames.** A version already published is reused,
   GitCode's copy first because GitCode cannot replace an asset, so a republish
   converges on bytes that are already fixed; a fresh pack is reproducible.
   This is the rule xim-pkgindex adopted on 2026-09-05, and it needs no new
   asset name.
4. **GitCode's raw endpoint answered 403 for a while** for the mcpp-index
   pointer through `main`, and 200 by commit id, while xim-index's pointer
   answered 200; the responses carry Huawei WAF cookies, and the same request
   answered 200 later. A client whose CN pointer fetch fails falls back to the
   git source rather than to the GLOBAL artifact base; that is xlings'
   behaviour, recorded here and reported as openxlings/xlings#598, not changed in
   this batch. It is no regression: before the region object a CN client fetched
   the pointer from GitHub, and its fallback was the same git source.
5. **R2 also excuses the toolchain's own std module initialisers.** GCC 16's
   `libstdc++.so.6` exports `_ZGIW3std` and `_ZGIW3stdW6compat` itself, so the
   one-plan-object rule alone left one finding on the gcc default; names that
   `std.o` defines are also excused when the other provider lives inside the
   compiler's installation (W1).
6. **R3's placement skips layer providers and distribution packages**
   (`provides = ["mcpp:..."]`), which would otherwise report a graph C++ runtime
   as a two-image conflict (e2e 690 passes), and the root's reach includes its
   dev-dependencies. The criterion is a foreign `dlopen` with `RTLD_NOW` rather
   than a replayed `-z defs` link (W1).
7. **G4 needs `targetsInferred`.** A package with only `src/main.cpp` and no
   `[targets]` infers a program target; the rule applies to declared targets
   only, so the manifest records which of the two it produced (W2).
8. **G4's first-repetition refusal also covers an unconditional
   `[build-dependencies]` edge** on a tool that depends on its declarer, which is
   a genuine cycle (W2).
9. **G5 selects members without the adoption warning**: the root and a member
   pinned by the same revision are two identities over one clone, and the
   member's source string carries a `#member=` suffix (W2).
10. **S4's profile rule lives in `mcpp.build.prepare`.** Importing
    `mcpp.cli.cmd_build` from `mcpp.cli.cmd_publish` made GCC 16.1 crash at
    `import mcpp.cli;` on every build (W3).
11. **S2's shape value is `PeLld`, not `WindowsLld`**: the substring `wsl` is
    reserved by the runtime-contract source check (W3). A Windows host building
    a non-PE target whose driver is not named by `--target` keeps today's lld
    line, which keeps the Windows-to-Linux cross job unchanged.
12. **S3 uses one strip tool for every Android leg**: the NDK's `llvm-strip`
    reads every Android ABI (W3).
13. **P3 reads the environment rather than new accessors**, so `dist-apk` keeps
    working on an engine that does not publish the variables, and writes debug
    files per ABI, because one APK carries one library name per ABI (plugins).
14. **P4 serves the iOS device row as well**, as `rules-metal` does; the
    consumer declares the `@_cdecl` function itself rather than including the
    generated header, whose C++ visibility depends on the Swift version
    (plugins).

15. **F2 is confirmed, and answered with a diagnostic (macos-15, run
    35032727668).** Under the payload's Mach-O default a `std::runtime_error`
    thrown in a dylib is not caught by its class in the program and two
    `std::error_code` categories compare unequal; a host-coupled graph catches
    it and compares equal; the mixed leg (a self-contained program over a
    host-coupled library) splits as well. The default is unchanged in this
    release, and a build whose program loads a C++ dylib of its own is told
    once through `build/cxx-runtime-identity`, with `cxx_runtime =
    "host-coupled"` as the remedy. Changing the Mach-O default belongs to its
    own record, as T§4.4 states.
16. **The macOS stream-init shim was prepended to every link unit.** The object
    calls libc++'s `ios_base::Init` constructor, and a C-only shared library
    links without the C++ runtime, so on macos-15 the F1 fixture failed with
    `ld64.lld: error: undefined symbol: std::__1::ios_base::Init::Init()`. The
    shim now follows the predicate the link line itself uses
    (`unit_needs_cxx_runtime`). The defect predates this batch; no fixture had
    a C-only shared library beside a C++ program on macOS before e2e 702.
18. **The local verification of the integrated branch** (Linux x86_64, this
    host): the full unit suite is 121 of 121, and `tests/e2e/run_all.sh` reads
    `E2E Summary: 398 passed, 0 failed, 47 skipped`, the skips being the
    capabilities this host lacks (msvc, mingw, qemu, a device). The macOS and
    Windows legs are the pull request's own CI.
17. **`timeout` is not on a macOS runner.** e2e 732 bounds its own commands
    with `timeout`, `gtimeout` or neither, since the bound under test is the
    engine's.

### 1.10 The review before the release, and what it changed

The pull requests were reviewed once more after #650 merged and before
2026.9.16.1 was tagged, by two readers given the diff and the classes of defect
this project's CI has historically missed. Four findings survived measurement;
the release dispatch was cancelled to carry the first of them, so 2026.9.16.1
ships from the follow-up branch rather than from `f4529b2a`.

19. **A gate refused what the previous release builds** (engine,
    mcpp-community/mcpp#651). E4.2 compares a `[feature-deps]` restatement's
    source with the declaration in effect. A path is normalised first; a version
    constraint was compared byte for byte, so `">= 11.0.0"` and `">=11.0.0"`
    read as two sources. Measured: the manifest builds on the released
    2026.9.15.2 with no error and no warning, and is refused on `f4529b2a`. The
    judgement is now made with the constraint's whitespace removed, while the
    message shows each declaration as it was written. This is the
    "a new gate refuses yesterday's build" shape again; the rule it breaks is
    that a gate refuses the thing it names and nothing else.
20. **The record claimed a call the code does not make** (engine, same pull
    request). The first custom-index sync restates two of `decide_for_miss`'s
    conditions rather than calling it, which is correct -- that pair's debounce
    and one-sync-per-process guard are about the index that resolves a
    dependency, while this sync creates a local copy of a different set of
    repositories that nothing else will create, so taking the guard would let an
    earlier refresh suppress a clone the build cannot proceed without. The
    comment said "the policy's answer is taken" without saying which half, and
    now states both halves and the reason.
21. **Two contributors naming one file: the deepest won** (plugins,
    mcpp-community/mcpp-plugins#28). `contributions` is ordered highest priority
    first, and the resources merge walks it backwards for exactly that reason.
    The assets merge walked it forwards into a copy that overwrites, so the
    deepest dependency decided a file two packages name. Measured on the new
    `tests/apk-consumer-graph/lib2` fixture: `from-the-deeper-library` where the
    documented order gives `from-the-requester`. Separately, `lib/<abi>/` is
    flat, so two contributors carrying one library name produced two build steps
    with one id and one output; one destination now has one claimant, the first
    in the priority order, and a later claim is reported by name.
22. **A reader that could not represent what it accepted** (plugins, same pull
    request). `mcpp::plugins::json` decoded each `\u` escape on its own, so a
    code point above U+FFFF -- which reaches JSON as a surrogate pair -- became
    two three-byte sequences holding unpaired surrogates, which is not UTF-8.
    Pairs are combined now, and a surrogate that is not half of one is refused.
    mcpp's own writer escapes only characters below `0x20` and passes UTF-8
    through, so no producer reaches this path today: the reader is shared by
    `dist-apk` and `dist-apple`, and the fix is that it refuses what it cannot
    represent rather than writing it into a manifest.

## 2. Engine tasks

### 2.1 Lead: #648 (L1 to L7)

- **L1.** `decide_for_dependency` calls `legacy_bare_candidates` under
  `spec.isVersion() && spec.namespaceOmitted` after a conclusive miss, and
  treats a hit as `None`. Unit: `IndexRefresh.LegacyBareRung`. e2e 730: a
  fixture whose bare dependency resolves through the rung, planned offline with
  `-v`, prints no suppressed decision.
- **L2.** `StdoutToStderr` saves with `F_DUPFD_CLOEXEC` (Windows: the saved
  CRT descriptor's handle is made non-inheritable). e2e 731: reading A2's
  build program lists no descriptor naming the reader's pipe.
- **L3.** `call`, `update_index_unguarded`, `install_direct`, the bootstrap
  and `config` invocations run through the owned launcher with argv, the
  environment the `ScopedInvocationEnv` states, and the working directory as
  a parameter. The refresh is bounded by `[index] refresh_timeout` (default
  120 seconds); an install by an inactivity bound of 300 seconds without an
  NDJSON line. A timeout is not retried. e2e 732: a stub xlings that sleeps;
  `emit build-database` with local data returns within the deadline plus a
  margin and succeeds; after SIGTERM to mcpp no stub process remains.
- **L4.** `mcpp::wire` keeps a per-run set of observed effects; the launcher
  of a network child adds `network`. Every enveloped command merges it into
  its `effects`. e2e 733 (with 732's stub): a refresh run's envelope lists
  `network`; an offline run's does not.
- **L5.** The fetcher's pre-install refresh, its refresh before a retry, and
  the first custom-index sync call `decide_for_miss` and apply its decision.
  e2e 734: `auto_refresh = false` with a missing package runs no `update` in
  the stub's argv log and names `mcpp index update`.
- **L6.** `refusal::Code::OfflineDownloadRequired` at the five offline sites and
  the absent-index case; `emit build-database` maps it to
  `MCPP_OFFLINE_DOWNLOAD_REQUIRED`. e2e 735.
- **L7.** The configuration template, the in-memory default and the
  `.xlings.json` migration write `artifact` as
  `{"GLOBAL": "https://github.com/xlings-res/mcpp-index", "CN": "https://gitcode.com/xlings-res/mcpp-index"}`.
  Unit: the migration is idempotent and preserves unrelated state.

### 2.2 W1: images and runtimes (R1 to R5)

- **R1.** In the contract resolution, when the root image of a plan links a
  `SharedLibrary` unit of the plan whose objects are C++, the `Distributable`
  and `Test` roles on an ELF target take the `SharedLibrary` role's contract.
  An explicit `cxx_runtime = "self-contained"` (or `{ program = ... }`) in
  that graph is refused with `program-cxx-runtime-split`. e2e 700 (llvm and
  gcc legs): the default shape runs; the explicit statement is refused.
- **R2.** `STB_GNU_UNIQUE` joins `STB_WEAK` as vague linkage; the check
  receives, per image, the plan objects linked into it and suppresses a
  duplicate defined by one object in both. Unit tests on both rules. e2e 701:
  a uniform-contract program over a C++ shared library reports no
  `build/symbol-provision` finding and `--strict` exits 0; a program with a
  static zlib over a shared libz still reports.
- **R3.** `make_plan` computes each shared image's static closure over
  `directPackageDeps`; a static package in exactly one closure is linked into
  that image and removed from the root's objects; one in several closures is
  refused (`static-package-in-two-images`) on Mach-O, PE and the Android app
  row, and diagnosed (degraded) on other ELF rows. Unit: the six shapes of
  §1.2. e2e 702: M3b's library links under `-Wl,-z,defs` and loads through a
  foreign `dlopen`; the two-closure shape builds with the diagnostic on Linux
  and without it after `linkage = "shared"`.
- **R4.** The PE contract table records `self-contained` for clang on the MSVC
  ABI unless a runtime flag is emitted; an explicit `host-coupled` or
  `toolchain-coupled` on that row is diagnosed; the `msvc_crt_flag` comment is
  corrected; docs/20 states the row's model. Unit test of the table. e2e 703
  (Windows shard): `resolution.json` records `self-contained`.
- **R5.** e2e 704 (macOS shard): the #641 M3 app/fw pair without
  `llvm.libcxx`, three legs, prints whether `std::runtime_error`, the
  library's own exception and an `std::errc` comparison cross the dylib
  boundary. e2e 705 (Windows shard): the same fixture over an llvm-row DLL.
  Both print `READING` lines; the decision of T§4.4 is taken from them in
  §1.9.

### 2.3 W2: features, tools and git sources (G1 to G6)

- **G1.** The forward validator reads the dependency tables of every row and
  feature (`conditionalConfigs`) and the build-dependency table. e2e 710.
- **G2.** `mergeActiveFeatureDeps` refuses a restatement whose path, git
  source or version differs from the declaration in effect on the row. docs/05
  and its mirror say to restate the source. e2e 711.
- **G3.** `published_names(provider, consumer)` is used by `fillDepDirs` and
  the tool publication. e2e 187 gains a `namespace =` plus `name =` leg.
- **G4.** Step 0 is the scan of §1.2. A package whose targets are all
  programs is not walked into a consumer's target graph and compiles nothing
  there; its tools come from the sub-build. Package-edge cycles are detected
  in the resolution pass with a `package-cycle` refusal naming the edges; the
  tool chain refuses a repeated (source, tool) at its first repetition. e2e 712
  (the issue's fixture with the default cache; E6c's link line; E6b refused
  once).
- **G5.** A git dependency whose identity is not the root manifest's is
  looked up among the root's `[workspace] members` at the same commit; a
  member's in-clone path edges resolve as the same git source; a second
  declaration of one dependency by one consumer merges `tools`, `features`,
  `host-module` and `reexport` into the edge; the banner prints the short
  commit. e2e 713.
- **G6.** A `/` token in `--features` is split and applied as a root forward;
  one naming no dependency warns (error under `--strict`) whether or not the
  root declares `[features]`, and never becomes a macro. `mcpp why deps`
  accepts `--features`. e2e 714.

### 2.4 W3: build programs, the link line and pack (S1 to S5)

- **S1.** Before the root build program, `prepare` writes
  `<build>/.mcpp/graph.json` (the `resolution.json` `graph` objects plus
  `manifest_dir`, `features`, `targets` kinds, `link`, `metadata`) in
  dependency order, and publishes its path as `MCPP_GRAPH_FILE`
  (`mcpp::graph_file()`); its digest joins the contract environment. The
  manifest parser keeps `[package.metadata]` as a raw table. e2e 720.
- **S2.** `link_shape(host, target_format, dialect)` selects the branch;
  `tests/unit/test_build_flags.cpp` gains host-by-row cases for macOS and
  Windows hosts. e2e 721 (macOS shard): `mcpp build --target
  x86_64-linux-android` links an ELF object.
- **S3.** `pack::Plan` carries the graph's `SharedLibrary` outputs;
  `run_shared_program` strips per leg; `bundle_libs`, `stage_closure` and the
  multi-leg path strip graph-built libraries and toolchain runtime copies with
  `--strip-unneeded`; the status line prints what was done;
  `MCPP_PACK_STRIP` and `MCPP_PACK_DEBUG_SYMBOLS_DIR`. e2e 722 (Linux desktop
  and, where the NDK is present, Android).
- **S4.** `pack` accepts `--release`, `--dev` and `--message-format json`;
  the `mcpp.pack` kind is in `kKinds` and `--protocol-version`; `run` resolves
  the profile with `build`'s function. e2e 723.
- **S5.** `[package]` reports an unknown key the way `[build]` does, with
  `metadata` known. Unit test in the manifest suite.

## 3. Plugin tasks

One pull request on mcpp-plugins, version 0.12.0, after the engine release
(P3 and P5 need the released accessors; P1, P2 and P4 do not, and are written
first). Its CI runs the consumers against the pinned engine and, before the
release, against `mcpp_ref=feat/646-649`.

## 4. Index and project tasks

- **X1** is independent of the engine and merges first; the next index publish
  republishes the artifact, and the group d probe of the triage record then
  reads three equal digests.
- **X2** is the release bot's pull request.
- **X3** follows X2 and the plugins tag.
- **X4** is a two-line manifest change in openxlings/xlings.

## 5. Order

1. X1 and X4 open at once (independent of everything).
2. W1, W2, W3 and the lead's L1-L6 in parallel; the lead merges each tree into
   `feat/646-649` as it completes, builds, and runs the unit tests and the new
   e2e scripts.
3. L7, C1 and C2 on the integrated branch; one pull request; CI green,
   including the measurement legs; §1.9 records R5's readings and the decision
   they imply.
4. Self-review of the diff and of the ecosystem; merge.
5. Release 2026.9.16.1; GitCode assets uploaded locally as each archive
   appears; X2.
6. P1-P5 on the plugins branch; its CI against the released engine; tag 0.12.0,
   mirrored to GitCode; X3.
7. V1; the bootstrap pin; V2; C3.

## 6. Release chain

`gh workflow run release.yml --ref main` from a clean `main` whose HEAD is the
merge commit, with nothing pushed to `main` until the tag exists. Each archive
and its `.sha256` go to `xlings-res/mcpp` on GitCode with the local tool as soon
as the build job puts it on the GitHub release; an asset counts as done when a
GET returns 200 with the upstream size and the bytes compare equal.
`publish-ecosystem` then opens the index pull request, whose four hashes are
compared with the downloaded archives by script. The release is complete when
the published xim index artifact (not git main) names 2026.9.16.1 as `latest`.

## 7. Sandbox verification (V1)

A new SubOS `v646`, entered with `xlings subos use v646 --sandbox --cmd`,
after `xlings config --mirror CN` and `mcpp self config --mirror CN` inside it.
The engine is addressed by its store path and exact version. The script runs
first against 2026.9.15.2 (change detectors fail, guards pass), then against
2026.9.16.1 (all pass). Scenarios, each with a `READING` line:

1. F3a: an llvm program over a C++ shared library runs.
2. F1: a shared package over a static package links under `-z defs`.
3. E6: a feature-gated tool that depends on its declaring package builds and
   runs; `dep_bin` answers under both spellings.
4. E8: `--features <dep>/<feature>` builds the dependency's tool.
5. E1: the root build program reads the graph file and a dependency's
   metadata.
6. E5: `mcpp pack --format tar` ships a stripped graph-built `libdep.so`.
7. E9: `mcpp pack --release --message-format json` prints a record whose path
   exists.
8. #648: offline `emit build-database` with a removed package yields
   `MCPP_OFFLINE_DOWNLOAD_REQUIRED`; a build program lists no inherited pipe;
   a bare `compat.*` dependency produces no refresh decision.
9. A6: `mcpp index update` with `mirror = CN` succeeds from GitCode.
10. plugins 0.12.0 from the index: `dist-web` with `page` names the page.

## 8. Parallel work trees

| tree | branch | owns | may touch minimally |
|---|---|---|---|
| lead `mcpp-646` | `feat/646-649` | `modules/platform/src/{terminal,process}.cppm`, `modules/platform/src/{unix,windows}/bounded_process.cppm`, `src/xlings/xlings.cppm`, `src/pm/index_refresh.cppm`, `src/pm/package_fetcher.cppm`, `src/config.cppm`, the refresh, custom-index and offline-refusal blocks of `prepare.cppm`, the `emit build-database` function of `cmd_build.cppm`, the effects half of `src/wire.cppm`, workflows, CHANGELOG, version files | `src/build/refusal.cppm` (one code) |
| W1 `mcpp-646-runtime` | `feat/646-649-runtime` | `src/build/distribution.cppm`, the link-unit assembly of `src/build/plan.cppm`, `src/build/symbol_provision.cppm`, `src/runtime/elf.cppm`, `src/build/runtime_validation.cppm`, `src/toolchain/dialect.cppm`, the CRT block of `flags.cppm`, docs/20, docs/22 | `src/build/refusal.cppm` (two codes), the contract call site in `prepare.cppm` |
| W2 `mcpp-646-graph` | `feat/646-649-graph` | the feature, forward, worklist, edge, git-source, tool-publication and cache-key-walk blocks of `prepare.cppm`, `src/build/execute.cppm` (banner), `src/build/tool_store.cppm`, the `why deps` option table in `src/cli.cppm`, docs/05 (features and git), docs/06, docs/07, the `dep_bin` section of docs/30 | `modules/manifest` (feature-deps check), `src/build/refusal.cppm` (one code) |
| W3 `mcpp-646-surface` | `feat/646-649-surface` | the root build program block and `resolution.json` writer of `prepare.cppm`, `src/build/hostprogram.cppm`, `src/build/build_program.cppm`, the link-branch block of `flags.cppm`, `src/pack/*`, the pack option table of `src/cli.cppm`, the `run` profile resolution of `cmd_build.cppm`, the kinds table of `src/wire.cppm`, `tests/unit/test_build_flags.cpp`, docs/10, the accessor sections of docs/30, the `mcpp.pack` section of docs/50 | `modules/manifest` (`[package]` keys) |

e2e numbers: W1 700-709, W2 710-719, W3 720-729, lead 730-739. A tree removes
its `target/` after its branch is merged.

## 9. Closure

### 9.1 What shipped

| repository | pull request | published as |
|---|---|---|
| mcpp-community/mcpp | #650 (`f4529b2a`), #651 (`2f925d48`) | 2026.9.16.1 |
| mcpp-community/mcpp | #652 (`074d87b3`) | the bootstrap pin |
| mcpp-community/mcpp-plugins | #28 (`dea1f09`) | 0.12.0 |
| openxlings/xim-pkgindex | #845 (`1c951f0b`) | mcpp 2026.9.16.1 as `latest` |
| mcpplibs/mcpp-index | #432 (`0cbac960`), #433 | the reproducible artifact; mcpp:plugins 0.12.0 |
| openxlings/xlings | #597 (`4ea4eac9`) | the `compat.*` spelling |

The engine is one pull request as the goal requires, and #651 is the exception
it allows: the review before the release found that #650 refuses a manifest
2026.9.15.2 builds, and the release was cancelled to carry the fix rather than
ship the cliff and repair it in a second version (§1.10 item 19).

The release is mirrored: each of the four archives was downloaded from GitCode
and compared with its published `sha256`, and all four match. The plugins
source archive was compared with `cmp` against the GitHub archive of the tag
and is byte-identical; `90a70689b090be72` names both, and the index descriptor
carries that digest.

### 9.2 What the sandbox verified

`.agents/docs/2026-09-16-646-649-verify.sh` in SubOS `v646`, with
`xlings config --mirror CN` and `mcpp self config --mirror CN` inside it,
against the PUBLISHED engine addressed by its store path.

The control against 2026.9.15.2 reads `fails=10`: every change detector fails
and every guard passes, which is what makes the run against 2026.9.16.1
evidence rather than decoration. The run against the published release reads
`version=2026.9.16.1 fails=0`, seventeen assertions:

    ok: mcpp --version says mcpp 2026.9.16.1
    ok: xlings config --mirror CN
    ok: mcpp self config --mirror CN
    ok: mcpp's xlings reads mirror CN
    ok: the mcpplibs artifact names the GitCode mirror
    ok: mcpp index update
    ok: the program runs on one C++ runtime
    ok: libfw.so loads on its own (RTLD_NOW): x is inside it
    ok: the tool builds, and dep_bin answers under both spellings
    ok: --features spike.fw/installer built the dependency's tool
    ok: the graph lists b before a, with b's [package.metadata]
    ok: pack --release --message-format json names an artifact that exists
    ok: the graph-built libdep.so is stripped
    ok: an offline plan that needs a download has its own code
    ok: A2: no planning child holds the caller's pipe
    ok: T: cjson@1.7.19 resolves through the rung and asks for no refresh
    ok: mcpp:plugins 0.12.0 resolves and a consumer builds

### 9.2.1 An index artifact reaches its publisher before its consumers

Section K failed twice after mcpplibs/mcpp-index#433 merged and its artifact
published. The sandbox read `mcpplibs@artifact:0cbac96` -- the artifact from
before the merge -- two minutes after `vc176883` was published as the latest
release. The same lag made mcpp-community/mcpp#652 report twelve red checks
while its pin was correct: those runners read `xim@artifact:ccc6e12` after
openxlings/xim-pkgindex#845 had merged and published.

Neither is a defect in the thing under test, and neither is visible from the
publishing side, where the new pointer is already served. A release is reachable
when a CONSUMER resolves it, so both were settled by resolving the package
through mcpp on a host outside the publishing path and only then re-running.

### 9.3 Four probe defects, and the shape they share

The first three runs against the published engine read `fails=4`, `fails=2` and
`fails=1`. Not one was an engine defect.

1. Sections E and F wrote `std::println` into a build program whose fixture
   states `standard = "c++20"`, where it does not exist. The build program
   failed to compile, and the section reported the engine.
2. Section G printed its reading to standard output. mcpp discards a build
   program's output when it exits 0, so the reading was never seen and the
   section reported the engine. Both now use `mcpp::warning`, which is what
   every e2e script that reads a build program already used.
3. Section I then failed where it had passed: the sandbox's `$HOME` persists
   between runs, a build program is cached by its source, and a cache hit does
   not re-run it or replay its warnings, so the probe measured the previous
   run. Every build program the script writes now carries a per-run token.

The third defect is the one worth keeping. Sections E and G passed on the run
where their sources had just changed, and would have failed on the next run for
the reason section I failed on that one. A probe that reads a build program has
to make the build program new, or it measures the run before it.