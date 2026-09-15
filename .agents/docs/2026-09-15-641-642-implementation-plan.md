---
subject: triage
status: landed
---

# #641 and #642 implemented: the plan, its review from eight angles, and the ledger

**Status:** landed on 2026-09-15 as mcpp 2026.9.15.2 (mcpp-community/mcpp#644),
`llvm.libcxx` 22.1.8.3 (mcpplibs/libcxx#1), `openkal-llvm-runtime` 0.9.7
(mcpplibs/openkal-llvm-runtime#22), openxlings/xim-pkgindex#843 and
mcpplibs/mcpp-index#428; §9 records the closure. This record turns the decisions of
`2026-09-15-641-642-link-forms-standards-and-paths.md` ("the triage record")
into work: one pull request per repository, the order in which they merge and
release, the criterion each task is held to, and a ledger whose rows are
closed only by a reading. The triage record's recommendations D1 to D4 are
adopted; §1 states the refinements this review adds before any code is
written, and a refinement found while implementing is appended there.

Base commits: mcpp `69fae268` (2026.9.15.1), mcpplibs/mcpp-index, mcpplibs/libcxx
`24d60821` (22.1.8.2), mcpplibs/openkal-llvm-runtime, openxlings/xim-pkgindex
at their heads on 2026-09-15.

## 0. The ledger

Status is `todo`, `doing`, `branch` (implemented on the pull request's branch,
with its reading), `done` (merged or published, with its reading) or `dropped`
(with the reason). Owner `lead` is the integrating session; `W1` to `W3` are
the parallel work trees of §8.

### 0.1 Engine: mcpp-community/mcpp, one pull request (`feat/641-642`)

| id | task (triage §) | owner | depends on | status |
|---|---|---|---|---|
| M1 | a required compiler family's version is taken from pins that name the same payload; the refusal no longer states a false reason (§3.1) | lead | - | done (#644, `55a856d2`): unit `RequiredFamilyPins.*`; M1 probe reads `llvm@30.0.16248370` on 2026.9.15.1 |
| M2 | `mcpp pack --features` reaches every build pass `pack` performs (§3.6) | lead | - | done (#644, `55a856d2`): e2e 689 (fails on 2026.9.15.1: `pack --features installer failed`) |
| M3 | a dependency's C++ shared library in a graph whose C++ runtime is a package is refused before compiling, unless `cxx_runtime` states `self-contained` for shared libraries; under that statement the library links the provider's objects (§3.3) | lead | - | done (#644, `55a856d2`): e2e 690, four legs (fails on 2026.9.15.1: the refusal is absent) |
| M4 | `linkage = "static" \| "shared"` in `[targets.<n>]` and `[target.<sel>.targets.<n>]` states the package's default form; precedence and the information line (§3.4) | W1 | - | done (#644, `55a856d2`): unit `LinkageForm.*`, manifest cases; e2e 692 (fails on 2026.9.15.1: no libfw.so for a silent consumer) |
| M5 | dependency link forms are computed once before the root's build program and applied where they are today; `MCPP_DEP_<NAME>_LINKAGE` and `mcpp::dep_linkage` for the root's program (§3.5) | W1 | M4 | done (#644, `55a856d2`): e2e 693 (fails on 2026.9.15.1: `'dep_linkage' is not a member of 'mcpp'`) |
| M6 | a C++-layer provider that states `[package] standard` compiles its implementation units at that level; module units stay at the graph's (§3.2) | W2 | - | done (#644, `55a856d2`): unit `CxxLayerStandard.*` (9), `CacheKey` case; e2e 696 (fails on 2026.9.15.1: `new.cpp` at `-std=c++20`) |
| M7 | the object address of a source outside its declaring package is relative to its owning package, or a hashed directory when no package owns it (§3.7 item 3) | W3 | - | done (#644, `55a856d2`): unit `ObjectAddress.*`; e2e 698 leg A (fails on 2026.9.15.1: `obj/installer/__up/__up/dep/...`) |
| M8 | the host-tool sub-build runs in a short key-named scratch directory (§3.7 item 2) | W3 | - | done (#644, `55a856d2`): unit `ToolStoreScratch.*`; e2e 698 leg B |
| M9 | files the engine opens from ninja-invoked subcommands go through one extended-length path helper on Windows (§3.7 item 1) | W3 | - | done (#644, `55a856d2`): unit `PlatformFs.*`; e2e 698 leg C (Windows readings from CI) |
| M10 | CI: the llvm-dependent e2e scripts of M3 and M6, and the existing 663, run on a job that has llvm, with their PASS lines asserted (§1.8) | lead | M3, M6 | done (#644, `55a856d2`): `ci-linux-e2e.yml` hermetic job step |
| M11 | user documentation with its Chinese mirror; SPEC changes; CHANGELOG; version 2026.9.15.2 | lead, W1-W3 | M1-M9 | done (#644): docs 04, 07, 10, 20, 22, 30, 50 with mirrors; SPEC-001 v1.4 criterion 9; CHANGELOG; 2026.9.15.2 |
| M12 | the triage and plan records closed with their readings | lead | all | done: this revision, §9 |

### 0.2 Packages: mcpplibs/libcxx and mcpplibs/openkal-llvm-runtime, one pull request each

| id | task | owner | depends on | status |
|---|---|---|---|---|
| P1 | `llvm.libcxx` 22.1.8.3: `[package] standard = "c++23"` replaces `[build] cxx_standard`; a CI leg builds a c++20 consumer with the released engine | lead | M6 released | done: mcpplibs/libcxx#1 (`2aa6724d`); CI green on five rows under 2026.9.15.2, and before the release under `mcpp_ref=feat/641-642` (run 34896741837) |
| P2 | `openkal-llvm-runtime` next patch: the same statement | lead | - | done: mcpplibs/openkal-llvm-runtime#22 (`246bc669`), 0.9.7; green under its pin and under `mcpp_ref=feat/641-642` (run 34895002497) |
| P3 | both tags published and mirrored to GitCode | lead | P1, P2 | done: tags `22.1.8.3` and `0.9.7`; each GitHub archive and its GitCode mirror byte-identical (sha256 `b7360797…`, `8914579f…`) and opened as a tar |

### 0.3 Index and payloads

| id | task | owner | depends on | status |
|---|---|---|---|---|
| I1 | openxlings/xim-pkgindex: the release bot's `mcpp` bump merged | lead | release | done: #843 (`15d78b85`), +22/-3, four hashes equal to the downloaded archives; pointer `index_version` `15d78b8`; the host installed `mcpp@2026.9.15.2` through the CN mirror |
| I2 | mcpplibs/mcpp-index: `llvm.libcxx` 22.1.8.3 and the `openkal-llvm-runtime` patch; the index CI's `MCPP_VERSION` moves to 2026.9.15.2 | lead | P3, I1 | done: #428 (`83801b98`); the artifact published; the full member sweep under 2026.9.15.2 green after #429 (§9.4) |

### 0.4 Verification

| id | task | owner | depends on | status |
|---|---|---|---|---|
| V1 | a fresh SubOS sandbox with CN mirrors for xlings and mcpp runs the scenarios of §7 against the published engine and index | lead | I1, I2 | done: SubOS `v641`, 18 of 18, `fails=0` (§9.3) |
| V2 | replies on #641 and #642 naming the release and what the framework writes | lead | V1 | done: issuecomment-5675298116 and -5675298352 |

## 1. The review

The triage record held each decision to what the code does. This section holds
the set of decisions to eight further questions, and records what changed.

### 1.1 Architecture

- **One derivation per decision.** M5 does not add a second computation of the
  link form: the existing resolution is split into a pure computation and its
  application, and the computation moves. Its one input the scan produced,
  whether a package has sources, comes from a function factored out of the
  scanner, so the scan and the resolution read the same enumeration.
- **The per-unit level of M6 is one value.** The plan appends the dialect's
  spelling of the provider's level to the unit's own flag list, which every
  emitter already reads (compile edge, scan edge, `compile_commands.json`, the
  build database). No emitter learns a new field.
- **M3's two readings share one function.** Whether `cxx_runtime` states a
  contract for shared libraries is factored into the distribution module and
  read both by the flag assembly and by the refusal, so the two cannot disagree
  about what the user wrote.
- **M1 moves the comparison next to its data.** The pin selection leaves the
  lambda in `prepare.cppm` for the toolchain registry, beside
  `to_xim_package`, whose answer it compares.

### 1.2 Stability

Every change is additive or changes only builds that fail today, with two
exceptions stated here: M5 adds variables to the root build program's
environment, so each root program with dependencies re-runs once after the
upgrade; M7 and M8 move object and scratch paths, so the affected units compile
once more. No cache format, manifest meaning or command output changes for a
build that works on 2026.9.15.1.

### 1.3 Simplicity

- M4 adds one key and no new table; M5 adds one variable family and one helper
  shaped like `dep_dir`; M3 adds no key.
- M6 is scoped to the packages that need it (providers of the C++ layer), which
  keeps the one-standard rule intact for every other package and avoids a
  per-package standard mechanism nobody has asked for.
- M8 changes one path expression; M9 is one helper applied at enumerated call
  sites.

### 1.4 User experience

- The M3 refusal names the shared library, the provider, and the two remedies
  (`linkage = "static"` on the edge; `cxx_runtime = { shared =
  "self-contained" }` with its consequence).
- The M4 information line names both statements when a consumer overrides a
  package's default; `mcpp why deps` shows `package-default` or `requested`.
- M1's refusal, when no version exists at all, no longer claims that no row
  pins one while rows do.

### 1.5 Compatibility and seamless upgrade

- M4: engines up to 2026.9.15.1 ignore `[targets.<n>] linkage` with a schema
  warning (silent for a dependency) and link the package static, which is their
  default; they refuse a row table that has no string `kind`. A package that
  uses the row form states its engine floor, as the triage record says.
- M6 is byte-identical for every graph until a provider states `[package]
  standard`; P1 and P2 change only that statement, which older engines ignore
  for a dependency and read as c++23 at the package's own root, where c++23 is
  what they compile today. P1 and P2 may therefore be published before the
  engine without harm; they are published after it so that P1's c++20 leg can
  measure the combination.
- The bootstrap pin (`.xlings.json`) moves only after the release is indexed.

### 1.6 Cross-platform

- M9 is Windows-specific in effect and compiles on every host; its unit test
  runs everywhere (the helper is the identity outside Windows) and the Windows
  e2e is the criterion.
- M7 changes addresses on every host; its unit test runs on Linux, and the
  Windows e2e of the issue's shape covers the case that motivated it.
- M3 refuses on every format; its e2e runs on Linux (llvm job) and on the macOS
  e2e job, where llvm is the default toolchain.
- M4 and M5 e2e scripts use the host's default toolchain and run on the three
  e2e workflows.

### 1.7 Consistency

`linkage` is the word the consumer already writes; `dep_linkage` and
`MCPP_DEP_<NAME>_LINKAGE` sit beside `dep_dir` and `MCPP_DEP_<NAME>_DIR` with
the same names and sanitiser; M6 uses the C++-layer predicate the std module
adoption already uses; M3 reuses the existing `cxx_runtime` key with the meaning
it has for the payload's runtime.

### 1.8 Test coverage

The Linux e2e shards have no llvm, so a `# requires: llvm` script is skipped
there and `run_all.sh` exits 0. M10 runs the llvm scripts of M3 and M6, and 663
which has never run in CI, directly on the `hermetic` job (which already
installs `llvm@22.1.8`) and asserts each script's final PASS line, and the
line of the run step inside it.

### 1.9 Refinements found while implementing

- **M5's computation follows the layer-conditional pass (L1b), not only the
  dependency programs.** L1b changes dependency manifests, so a computation
  placed before it would read facts that are not final. The p1689 scanner had a
  source enumeration of its own that ignored `!` exclusions; it now reads the
  function the resolution reads. Form B descriptors read `linkage` as
  `mcpp.toml` does, and SPEC-001 records the precedence (v1.4, criterion 9).
- **M6's level enters the dependency cache key and the fingerprint** when a
  provider states one, because the graph's level alone no longer says which
  level the provider's sources were compiled at. The opt-in p1689 scan runs
  before planning and still preprocesses these units at the graph's level,
  which affects import detection only.
- **M7 and M8 shorten the issue's case to about 170 characters,** so crossing
  260 now needs nesting deep inside the dependency; e2e 698 leg C does that and
  is the Windows reading of whether ninja and the compilers cope. M9 had to
  guard two things beyond opens: a path string ninja reads back (the copied
  depfile's target keeps ninja's spelling) and an ANSI Win32 call
  (`CreateFileW` for the detached compile's log).
- **M3's static remedy depends on who made the library shared.** A package that
  constrains its form (`declaredShared`) cannot be linked static from its edge,
  so that remedy is offered only when a request or the package's default
  decided, and the refusal names the package's statement otherwise. `mcpp run
  --format` built its pack without the run's features; it now passes them.
- **On Mach-O a shared library over a graph runtime exports only what it marks.**
  `graph_runtime_compile_flags` compiles every unit of such a graph with hidden
  visibility on Mach-O, so that the runtime's instantiations are never coalesced
  with the system's libc++. Measured on macos-15 through `llvm.libcxx`'s CI
  under this branch: the private copy links the dylib, and the program's link
  then finds none of the library's functions until the declarations carry
  `[[gnu::visibility("default")]]`. A framework that already marks its API for
  its CMake shared build is unaffected; the rule is documented beside the
  refusal (docs 20).
- **`dep_dir` and `dep_linkage` answer under the qualified name.** A package
  that writes `namespace = "ns"` and `name = "fw"` is `ns.fw` to its consumer,
  and `dep_dir("ns.fw")` read nothing although the reference promised the
  canonical spelling; the framework's rule asks `dep_linkage("huxerui.huxerui")`.
  Both are now published under the qualified name as well (e2e 693 leg F).
- **F3, recorded and not addressed.** The std module object is linked into every
  C++ image, so a program over a C++ shared library that imports `std` reports
  its module initialiser as provided twice (`symbol_provision`). This predates
  the work: under gcc on 2026.9.15.1 the same shape reports 882 symbols; under
  the private copy of M3 it reports two, `_ZGIW3std` and `_ZGIW3stdW6compat`.

## 2. Engine tasks

### 2.1 M1

`resolve_required_family` keeps its two sources. The vocabulary source calls a
new registry function, `pinned_versions_for(const ToolchainSpec&)`, which keeps
a pin when `to_xim_package(pin).ximName` equals the requirement's. Unit test:
over `known_targets()`, `llvm` -> 22.1.8, `emsdk` -> 6.0.9, `android-ndk` ->
30.0.16248370, `gcc` -> 16.1.0. The e2e is not run on a fresh home in CI (it
would download a toolchain); the release verification V1 runs it.

### 2.2 M2

`pack` registers `--features`; `pack::Options` carries it; the three
`BuildOverrides` constructions in `pack/pipeline.cppm` and
`pack/library_pipeline.cppm` set it. e2e: a root with a feature-gated
`tools = [...]` dependency whose build program requires `dep_bin` only when
the feature is active; `pack --features` builds the tool, `build` without it
builds none.

### 2.3 M3

After `make_plan`, when the plan's C++ layer comes from the graph, each
dependency-owned shared link unit that holds a C++ object is checked. Under an
explicit `self-contained` shared contract the provider package's objects are
appended exactly as the program's link unit takes them; otherwise the build is
refused. e2e (Linux llvm job, macOS): refusal text; `linkage = "static"`
builds and runs; the explicit key links a library with no undefined
`std::__1` reference and the program runs.

### 2.4 M4 and M5 (W1)

Manifest: `Target` gains the default form and the statement that declared it;
`[targets.<n>] linkage` and the row form parse into it; in one table `kind =
"shared"` with `linkage` is refused. Merge: a later matching statement replaces
the earlier one, `kind` and `linkage` alike. `linkage_form`: `PackageFacts`
carries the default; `resolve` applies edge request, then written whole-graph
request, then the package default, then static; `Resolution` carries the
information line. M5: the resolution computation moves before the root build
program; `BuildProgramEnv` gains `depLinkages`, emitted under the names of
`depDirs`; `hostprogram` gains `dep_linkage`. Unit tests in
`test_linkage_form.cpp` and `test_manifest.cpp`; e2e for both.

### 2.5 M6 (W2)

`make_plan` identifies provider packages by the C++-layer predicate (factored
into the manifest module), reads their normalised `[package] standard`, and for
each of their compile units that neither provides nor imports a module and is
compiled as C++, appends `std_flag_for(tc, level)` to the unit's C++ flags when
the level differs from the graph's. The dependency cache key includes the
effective level. The higher-standard diagnostic does not report a provider
whose level is applied. Unit test in `test_build_flags.cpp` or a plan test; e2e
on the llvm job (a path copy of `llvm.libcxx` with the statement under a c++20
root). Documentation: `docs/07-workspace.md` §4.2 and its mirror.

### 2.6 M7, M8 and M9 (W3)

M7 in `plan.cppm` `object_for`; M8 in the host-tool branch of `prepare.cppm`
(`sub.work_dir`); M9 as `mcpp::platform::fs::extended_length` (identity outside
Windows) applied to the opens of `mcpp dyndep`, `mcpp stage`, `mcpp bmi-equal`,
`mcpp bmi-compile`, `mcpp bmi-await` and `mcpp coff-def`. Unit tests for M7 and
the helper; a Windows e2e of the issue's shape, which also prints the
`LongPathsEnabled` value and the longest path it produced as readings.

## 3. Package tasks

P1: `mcpplibs/libcxx` pull request with the manifest statement, version
22.1.8.3, the README line, and a CI leg that builds `examples/import-std` with
its root at c++20 under the released engine. P2: `mcpplibs/openkal-llvm-runtime`
pull request with the statement and a patch version. P3: tags, GitCode mirrors
(`mcpp-res/<repo>`), and byte comparison of the published archives.

## 4. Index tasks

I1: the bot's pull request on `openxlings/xim-pkgindex` merged with the
maintainer account, then the published index artifact read back. I2: one
pull request on `mcpplibs/mcpp-index` adding both package versions (sha256
computed from the downloaded archives) and moving `validate.yml`'s
`MCPP_VERSION`; the artifact read back after merge.

## 5. Order

1. W1, W2, W3 and the lead's M1-M3 in parallel; the lead merges each branch
   into `feat/641-642` as it completes, builds, and runs unit tests and the new
   e2e scripts.
2. M10 and M11 on the integrated branch; one pull request; CI green.
3. Self-review of the diff and of the ecosystem; merge.
4. Release 2026.9.15.2; GitCode assets uploaded locally as each archive
   appears; I1.
5. P1, P2, P3, then I2.
6. V1; the bootstrap pin; V2; M12.

## 6. Release chain

`gh workflow run release.yml --ref main` after the merge, from a clean `main`
whose HEAD is the merge commit. Each archive and its `.sha256` are uploaded to
`xlings-res/mcpp` on GitCode with the local tool as soon as the build job puts
it on the GitHub release; each asset counts as done when a GET returns 200 with
the upstream size. `publish-ecosystem` then opens the index pull request.

## 7. Sandbox verification (V1)

A new SubOS `v641`, entered with `xlings subos use v641 --sandbox --cmd`,
after `xlings config --mirror CN` and `mcpp self config --mirror CN` inside it.
The engine is addressed by its store path and exact version. Scenarios, each
with a READING line:

1. a root with `requires = ["mcpp:compiler=llvm"]` resolves `llvm@22.1.8`;
2. a c++20 root over `llvm.libcxx` 22.1.8.3 from the index builds and runs;
3. a shared path dependency over `llvm.libcxx` is refused with the remedies;
   with `cxx_runtime = { shared = "self-contained" }` it builds and runs;
4. a package with a default `linkage = "shared"`: a silent consumer gets the
   shared library, an explicit `linkage = "static"` gets the static form, and
   the root's build program prints the matching `dep_linkage`;
5. `mcpp pack --features` is accepted;
6. `openkal-llvm-runtime` at its new version resolves from the index.

## 8. Parallel work trees

| tree | branch | owns | may touch minimally |
|---|---|---|---|
| lead `mcpp-641` | `feat/641-642` | `src/toolchain/registry.cppm`, `src/cli.cppm` (pack), `src/cli/cmd_publish.cppm`, `src/pack/*`, `src/build/distribution.cppm`, the M3 block in `prepare.cppm` after `make_plan`, workflows, CHANGELOG, version files | `prepare.cppm` near `:7733` |
| W1 `mcpp-641-link` | `feat/641-642-link` | `modules/manifest/src/{types,toml}.cppm` (targets), `src/build/linkage_form.cppm`, `src/build/build_program.cppm`, `src/build/hostprogram.cppm`, `src/modgraph/scanner.cppm` (enumeration), the resolution block and root build program block of `prepare.cppm` | `prepare.cppm` merge of row kinds (`:511`) |
| W2 `mcpp-641-std` | `feat/641-642-std` | the compile-unit loop of `plan.cppm`, `src/build/cache_key.cppm`, the higher-standard diagnostic and std-module adoption predicate in `prepare.cppm` | `modules/manifest` (the predicate) |
| W3 `mcpp-641-paths` | `feat/641-642-paths` | `object_for` in `plan.cppm`, `modules/platform/src/fs.cppm`, `modules/dyndep`, the `stage`/`bmi-*`/`coff-def` subcommand sources, the host-tool block of `prepare.cppm` | - |

e2e numbers: lead 689-691, W1 692-695, W2 696-697, W3 698-699. A tree removes
its `target/` after its branch is merged.

## 9. Closure

### 9.1 The engine pull request

mcpp-community/mcpp#644 ran CI twice.

- **Round 1: three red jobs, none of them in the engine.** On all three hosts
  `00_fixture_path_hygiene.sh` refused e2e 696 and 698, which wrote manifest
  paths without `host_path`. On windows-2022 e2e 698's consumer passed a command
  with two quoted words to `std::system`, and cmd.exe stripped the outer quotes
  ("The filename, directory name, or volume label syntax is incorrect"). Both
  were fixed in the scripts, together with the refinements measured meanwhile
  (§1.9: the qualified dependency name, the Mach-O visibility rule).
- **Round 2: 40 checks green.** The hermetic job printed the asserted lines of
  663, 690 (`READING private copy: runtime_error not matched by its class`) and
  696. On windows-2022 e2e 698 read `LongPathsEnabled: 1`, an estimated longest
  sub-build path of 279 characters, and a deep tool build that exits 0.
- An independent read-only review of the diff against §2 and the triage record
  reported no defect.
- The squash merge `55a856d2` is byte-identical to the tested head `0cde03d2`.
  Every workflow on `55a856d2` is green; `ci-fresh-install` waited twenty
  minutes for the index, timed out before the bump merged, and passed on its
  second attempt.

### 9.2 The release

Release run 34900385287 built the four archives, sealed the manifest and ran
`publish-ecosystem`. Each archive and its `.sha256` were uploaded to GitCode from
this host as the build job published them; each counted as done at a GET of 200
with the upstream size, and the GitHub mirror answered the same four sizes.
openxlings/xim-pkgindex#843 was merged after its four hashes were compared with
the archives downloaded from GitCode; the pointer read `index_version 15d78b8`,
and `xlings install mcpp@2026.9.15.2` through the CN mirror installed a binary
that reports 2026.9.15.2.

### 9.3 The sandbox

`2026-09-15-641-642-verify.sh`, run in a fresh SubOS `v641` with CN mirrors for
xlings and mcpp, against the published engine and index: 18 of 18, `fails=0`.

| section | reading |
|---|---|
| A | `mcpp --version` 2026.9.15.2; both mirrors CN, read back from mcpp's xlings configuration |
| B | `llvm.libcxx` 22.1.8.3 and `openkal-llvm-runtime` 0.9.7 are in the index |
| C | a required llvm on a fresh home resolves `llvm@22.1.8` |
| D | a c++20 program over `llvm.libcxx` 22.1.8.3 builds, the report names the package as the C++ layer, and the program prints `1-2` |
| E | a shared dependency over the graph runtime is refused naming the private copy; with the statement it builds and runs |
| F | a package default `linkage = "shared"`: the silent consumer gets the shared library and its build program reads `shared`; an explicit `linkage = "static"` under `--strict` gets no shared library, the information line, and reads `static` |
| G | `mcpp pack --format dir --features extra` succeeds |

The same script run before the package revisions were indexed (SubOS
`v641pre`, `llvm.libcxx` 22.1.8.2) differs in D alone: the c++20 program stops at
`no template named 'bad_expected_access'`. That run is the control: the engine
change needs the package's statement, and the statement is what 22.1.8.3 adds.

### 9.4 The index members under the new engine

mcpplibs/mcpp-index validates only the descriptors a change names on a pull
request or a push, so the full member sweep under `MCPP_VERSION` 2026.9.15.2
was dispatched by hand.

- **Run 34933120467 on `83801b98`:** every member built and tested on Linux
  (default and llvm), macOS and Windows except `nanodbc` on the two Linux legs.
  Both stopped before compiling, on `compat.unixodbc`'s download:
  `connecting… 306s` and `download artifact missing`. The descriptor named only
  `www.unixodbc.org`, which did not answer from the runners or from a host in
  CN; the weekly sweep of 2026-09-13 had passed, so neither the member nor the
  engine had changed.
- **The fix, mcpplibs/mcpp-index#429 (`0d278e98`):** upstream publishes the
  same dist tarball as the asset of its GitHub release `v2.3.14` (sha256
  `4e2814de…`, equal to the descriptor's, with `libltdl/`). The descriptor
  names it as GLOBAL and a new GitCode mirror, `mcpp-res/unixodbc` release
  `2.3.14`, byte-identical, as CN.
- **Run 34939202738 on `0d278e98`:** 27 jobs successful and one skipped (the
  job that runs only when a sweep fails); `nanodbc 13s ok` on both Linux legs.
  Every member of the index builds and passes its tests under 2026.9.15.2.

### 9.5 What remains open

- **F1**, a shared package over a static package: recorded in the triage
  record §3.3.5, for its own record.
- **F2**, the payload's Mach-O shared default: `llvm.libcxx`'s CI measured the
  catch-by-type property for the graph's runtime on macOS arm64
  (`runtime_error not matched by its class`), as libc++'s comparison rules
  predict; the payload's origin is still unmeasured and touches builds that work
  today.
- **F3**, the std module initialiser linked into every C++ image (§1.9).
