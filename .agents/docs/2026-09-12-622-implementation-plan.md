---
subject: targets
status: active
---

# Implementation plan: a UI framework on Android, iOS and Web (#622)

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development
> to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** land every engine item of the design record in one mcpp pull request, then
the plugin, index and publication changes that depend on it, then verify the published
form in a sandbox.

**Architecture:** every engine item extends a mechanism that exists (the `abi` table,
the per-target `runtime` table, the closed `kind` set, the directive table, the
per-row naming table, the requirement check, the `platforms` validator, the
two-pass pack, the build-program environment). No item introduces a new class of
thing. The plugin members follow the dist-member shape; the index recipes follow the
`android-ndk` licence reading and the `simctl-run` session shape.

**Tech stack:** C++23 modules (`import std;`, `import mcpp.*;`), the bootstrap
`mcpp` at `~/.xlings/data/xpkgs/xim-x-mcpp/2026.9.12.2/bin/mcpp`, gtest unit tests
under `tests/unit/`, bash e2e scripts under `tests/e2e/` (`# requires:` header,
`MCPP=<absolute path>`), Lua recipes in `openxlings/xim-pkgindex`, `mcpp:plugins`
in `mcpp-community/mcpp-plugins`.

**Spec:** [2026-09-12-622-a-ui-framework-on-android-ios-and-web.md](2026-09-12-622-a-ui-framework-on-android-ios-and-web.md).
Section numbers below (§2.1 ...) refer to it. Each task's implementer reads its
section in full before writing anything.

## Global constraints

- One pull request per repository. The mcpp pull request carries every engine item
  (T1 to T7) and the documentation; it is not split.
- Prose in documentation, comments, commit messages and pull-request bodies is
  English, declarative, academic in register, without emoji. Each `docs/NN-*.md`
  change is mirrored in `docs/zh/NN-*.md`.
- Compatibility rule (§1 rule 6): a new key under `[targets.<name>]`,
  `[target.<sel>]` or `[target.<sel>.runtime]` is ignored with a warning by an older
  engine; a new `abi` member and `kind = "app"` are refused by name by an older
  engine. Nothing here changes how an older engine reads an existing manifest.
- Every criterion has a negative direction (§1 rule 8). A test that only asserts the
  positive case is incomplete.
- Every new build-program surface bumps `kProtocolVersion`
  (`modules/buildmcpp/src/program_protocol.cppm:79`, currently 10) once, to 11, in
  the task that adds the first directive; later tasks in the same PR reuse 11.
- Bootstrap: build with the 2026.9.12.2 binary; run unit tests with the fresh
  binary (`<fresh>/bin/mcpp test`); run each e2e with `MCPP=<absolute fresh path>`.
  Copy the fresh binary to a stable path before running a suite, so a rebuild does
  not replace the object under measurement.
- Before opening the PR: `git fetch origin main && git diff origin/main --stat &&
  git diff origin/main --diff-filter=D --name-only` (must be empty) `&& git log
  --oneline origin/main..HEAD`.
- The version is bumped in the same PR at the end (`mcpp.toml [package].version`
  and `src/version.cppm` in one commit) to the date of the release; the bootstrap
  pin in `.xlings.json` moves only after the release is indexed.

## Waves and file ownership

Tasks in one wave run in parallel, each in its own worktree branched from the
feature branch `feat/622-ui-framework-on-three-rows`, and are merged back in the
order listed. File ownership keeps merges small; a task that must touch a file
another task owns does so in a separate, minimal hunk.

| wave | task | owns |
|---|---|---|
| 1 | T1 wasm row (§2.5) | `modules/toolchain-model/src/triple.cppm` (naming), `src/build/flags.cppm` (flavour), `src/build/ninja_backend.cppm` (implicit output), `src/pack/pack.cppm` and `src/pack/stage_tree.cppm` (stem family), `tests/unit/test_artifact_naming.cpp` |
| 1 | T2 deploy directive and platform floor (§2.4, §2.11) | `modules/buildmcpp/src/directives.cppm` (new row), `modules/buildmcpp/src/program_protocol.cppm`, `src/build/hostprogram.cppm`, `src/build/build_program.cppm` (env), `src/build/plan.cppm:740-770` (absolute `from`) |
| 1 | T3 `kind = "app"` (§2.3) | `modules/manifest/src/types.cppm` (`Kind`), `modules/manifest/src/toml.cppm:1100-1130,1224,3409`, `modules/manifest/src/xpkg.cppm:1566`, `src/pack/route.cppm`, `src/build/plan.cppm:1373`, `src/build/prepare.cppm:1827,8405`, `modules/buildmcpp/src/directives.cppm:887-970` (Binary checks only), `src/build/execute.cppm` (run refusal) |
| 2 | T4 `exceptions` and per-target `requires_abi` (§2.1, §2.6) | `modules/manifest/src/toml.cppm:2752-2768,767-776,949-957`, `modules/manifest/src/types.cppm` (abi fields), `src/build/prepare.cppm:2856-2879,7455-7466,8909-8922`, `tests/unit/test_abi.cpp` |
| 2 | T5 `frameworks` per target and `platforms` (§2.2, §2.7) | `modules/manifest/src/toml.cppm:2787-2800`, `src/build/prepare.cppm:2256-2266`, `src/pack/library_pipeline.cppm:375-435`, `src/doctor.cppm:1032` |
| 2 | T6 `mcpp run --format` (§2.10) | `src/cli.cppm` (run and test options), `src/cli/cmd_build.cppm`, `src/build/execute.cppm` (operand), `src/pack/*` (the reported artifact as a return value) |
| 3 | T7 documentation, CHANGELOG, record status | `docs/04,10,21,22,30,31` and `docs/zh/*`, `CHANGELOG.md`, the design record's status line |
| 4 | T8 version bump, CI green, self-review, merge | `mcpp.toml`, `src/version.cppm` |
| 5 | X1 to X5 (index), P1 to P5 (plugins), I1 (mcpp-index), release, sandbox | other repositories |

---

## Wave 1

### T1: the Emscripten row of `artifact_naming`, the implicit `.wasm`, the stem family

**Files:**
- Modify: `modules/toolchain-model/src/triple.cppm:1082-1129` (`ArtifactNaming`, `artifact_naming`)
- Modify: `src/build/flags.cppm:756-767` (`LinkIntentFlavor` for `ObjectFormat::Wasm`)
- Modify: `src/build/ninja_backend.cppm` (the link edge for an Emscripten executable declares `bin/<name>.wasm` as an implicit output)
- Modify: `src/pack/pack.cppm` and `src/pack/stage_tree.cppm` (stage every file the link wrote with the executable's stem when the triple's object format is `Wasm`)
- Modify: the site that plans a `shared` link unit (find it with `grep -n "SharedLibrary" src/build/plan.cppm src/build/prepare.cppm`): refuse on `ObjectFormat::Wasm`, naming `-sSIDE_MODULE`
- Test: `tests/unit/test_artifact_naming.cpp`; new `tests/e2e/650_wasm_row_names_its_launcher.sh` (`# requires: emsdk` if that gate exists; otherwise a unit-level assertion on the plan plus a `# requires: elf` script that asserts the refusal of `shared` through `--target wasm32-emscripten` before any toolchain is resolved, if the refusal is reachable there)

**Interfaces:**
- Produces: `artifact_naming(Triple{os="emscripten"}, host)` returns
  `{exeSuffix=".js", libPrefix="lib", staticLibExt=".a", sharedLibExt=""}` with the
  shared-unsupported marker set; `LinkIntentFlavor::Wasm` exists and renders as
  `Elf` does. T6 and P3 rely on `${mcpp.target_file:<name>}` being `bin/<name>.js`.

- [ ] Step 1: add a failing case to `tests/unit/test_artifact_naming.cpp`: an
  `emscripten` triple on a Linux host naming yields `.js`, on a Windows host naming
  also yields `.js` (the host must not leak), `lib` prefix and `.a`; a `none` OS
  still falls back to host naming (the existing behaviour, asserted so the row does
  not widen the fallback).
- [ ] Step 2: run `<fresh>/bin/mcpp test artifact_naming`; expect the new case to fail.
- [ ] Step 3: add the `emscripten` branch before the fallback in `artifact_naming`,
  with a comment stating the industry reading (Emscripten's `Emscripten.cmake`
  `CMAKE_EXECUTABLE_SUFFIX ".js"`, Rust's `exe_suffix: ".js"`) and the engine's
  sentence (the executable is the file a runner executes, in the row's convention).
- [ ] Step 4: run the unit test; expect pass.
- [ ] Step 5: in `flags.cppm`, replace the fallback arm with `LinkIntentFlavor::Wasm`,
  rendered exactly as `Elf` (`-L`, `-l`), so the switch has no default arm; keep the
  comment that names `link_lib` as accepted by emcc (`-lidbfs.js`).
- [ ] Step 6: in `ninja_backend.cppm`, on the link edge of an executable whose triple
  has `ObjectFormat::Wasm`, add `| <bin>/<name>.wasm` as an implicit output. Assert in
  a unit test on the emitted `build.ninja` text if such a test shape exists
  (`grep -rl "build.ninja" tests/unit | head`); otherwise assert in the e2e.
- [ ] Step 7: in the pack staging, when the packed program's triple is `Wasm`, stage
  `<name>.wasm` (required, an error if absent) and every sibling
  `<name>.<anything>` the link directory holds (`.data`, `.worker.js`, `.wasm.map`)
  beside the launcher, and list them in the stage manifest. Write the rule in a
  comment: the launcher is the executable; the family is what the link also wrote.
- [ ] Step 8: refuse `kind = "shared"` on a `Wasm` triple at plan time with:
  `error: [targets.<name>] kind = "shared" is not supported on wasm32-emscripten: a side module needs -sSIDE_MODULE, which mcpp does not render`.
- [ ] Step 9: e2e `650`: with the emsdk payload available (`# requires: emsdk`, see
  how `tests/e2e/6xx` scripts that build for `wasm32-emscripten` gate themselves:
  `grep -l "wasm32-emscripten" tests/e2e/*.sh`), build a `1-2-3` program for
  `--target wasm32-emscripten`; assert `bin/<name>.js` and `bin/<name>.wasm` exist
  and no `bin/<name>` without extension exists; `mcpp run` prints `1-2-3`;
  `mcpp pack --format dir` stages both files and the `.stage-manifest` lists both; a
  `shared` target is refused with the sentence above; a `bin` whose main file has no
  `main()` links with `--no-entry` in `[target.'cfg(os = "emscripten")'.build] ldflags`.
- [ ] Step 10: run the e2e with `MCPP=<fresh absolute path>`; expect pass. Run the
  full unit suite.
- [ ] Step 11: commit: `wasm32-emscripten names its launcher .js and carries the .wasm as a link output (#622 A5)`.

### T2: `mcpp::deploy(from, to)` and `MCPP_TARGET_MIN_PLATFORM_VERSION`

**Files:**
- Modify: `modules/buildmcpp/src/program_protocol.cppm:79` (`kProtocolVersion = 11`)
- Modify: `modules/buildmcpp/src/directives.cppm:248-387` (a `deploy` row with a
  non-empty cache tag, `Scope::PackagePrivate` or the scope the `runtime` merge
  expects; a `Slot::Deploy`), `directives::apply` (append a `DeployEntry` to the
  package manifest's `runtimeConfig.linkIntent.deploy`)
- Modify: `src/build/hostprogram.cppm` (`inline void deploy(const char* from, const char* to)` beside `windows_subsystem`; `inline const char* min_platform_version()` beside `package_version`)
- Modify: `src/build/build_program.cppm:580-600` (`MCPP_TARGET_MIN_PLATFORM_VERSION` from `min_platform_version(...)` in `prepare.cppm:1581`, empty when the function returns empty)
- Modify: `src/build/plan.cppm:740-770` (a directive-sourced `from` may be absolute; the manifest-sourced path rule is unchanged)
- Test: `tests/unit/test_directives*.cpp` (find with `ls tests/unit | grep -i directive`), new `tests/e2e/651_a_build_program_deploys_what_it_generated.sh`, an assertion in an existing env-contract e2e (`grep -l MCPP_PKG_VERSION tests/e2e/*.sh`)

**Interfaces:**
- Produces: wire `mcpp:deploy=<from>\t<to>` (use the separator the table uses for
  two-argument rows, see `windows-subsystem`), `mcpp::deploy(from, to)`,
  `mcpp::min_platform_version()`, `MCPP_TARGET_MIN_PLATFORM_VERSION`. P2 and P4
  read the variable; P2 and P4 consumers call `deploy`.

- [ ] Step 1: unit test: a `Directives` parsed from `mcpp:deploy=<abs>/x.bin\tres` yields
  one `DeployEntry{from=<abs>/x.bin, to="res"}` on the manifest after `apply`; a
  `to` of `../x` is refused naming the directive; `protocol=11` is accepted and
  `protocol=12` refused (the existing protocol test shape).
- [ ] Step 2: run; expect failure on the unknown wire name.
- [ ] Step 3: add the table row, the slot, the `apply` arm, the accessor, the
  protocol bump. The comment on the row states: replayed from the cache record on a
  hit because the tag is non-empty; `from` may be absolute because it is an action
  output; `to` obeys `deploy_path_problem`.
- [ ] Step 4: run the unit test; expect pass.
- [ ] Step 5: in `plan.cppm`, accept an absolute `from` for directive-sourced entries
  (the manifest parser already refused absolute paths for manifest entries, so the
  plan's `absolute_from` must not re-root an absolute path). The copy edge's input
  is the absolute path, which is the action's declared output, so ninja orders the
  copy after the action.
- [ ] Step 6: `MCPP_TARGET_MIN_PLATFORM_VERSION`: add to the environment in
  `build_program.cppm` and to the accessor. Unit-test the environment builder if it
  has one (`grep -rn "MCPP_PKG_VERSION" tests/unit`), else the e2e.
- [ ] Step 7: e2e `651` (`# requires: elf gcc`): a build program declares an action
  writing `${out_dir}/gen/res.bin` (role `source`, a `cp` or `printf` via a small
  script in the fixture) and `mcpp::deploy(<that path>, "app.resources")`. Assert
  after `mcpp build`: `bin/app.resources/res.bin` exists with the generated
  content; touching the action's input changes the deployed file; deleting `bin/`
  and rebuilding (cache hit for the program) restores it, which is the replay
  criterion; `mcpp pack --format dir` stages `bin/app.resources/res.bin`;
  `to = "../x"` is refused naming the package. Also assert
  `MCPP_TARGET_MIN_PLATFORM_VERSION` is empty in the program's environment on Linux
  (print it to a file from the program).
- [ ] Step 8: run; expect pass. Run the unit suite.
- [ ] Step 9: docs are T7's; leave a note in the commit body listing the two surfaces.
- [ ] Step 10: commit: `build programs deploy what they generated, and read the platform floor (#622 A4, A11)`.

### T3: `kind = "app"`

**Files:**
- Modify: `modules/manifest/src/types.cppm:136` (`enum Kind { Library, Binary, SharedLibrary, TestBinary, Application }` plus `bool is_program() const` returning true for `Binary` and `Application`)
- Modify: `modules/manifest/src/toml.cppm:1106-1130` (parse `"app"`; the refusal text lists four kinds; `windows_subsystem`/`windows_entry` accepted for `Application` at `:1224`), `:3409` (target inference stays `Binary`)
- Modify: `modules/manifest/src/xpkg.cppm:1566` (descriptor reads `"app"`)
- Modify: `modules/toolchain-model/src/triple.cppm` (beside `artifact_naming`: `enum class ApplicationForm { Executable, SharedObject }; ApplicationForm application_form(const Triple&)` returning `SharedObject` when `env == "android"`, else `Executable`, with the comment from §2.3)
- Modify: `src/build/plan.cppm:1373` and every site listed in §2.3 (13 occurrences, `grep -rn 'Target::Binary\|Kind::Binary' src modules --include=*.cppm`): each "is this the program" question becomes `is_program()`; the link-unit builder maps `Application` to a shared link when `application_form(triple) == SharedObject` and to an executable link otherwise
- Modify: `src/pack/route.cppm:51,95` (`"app"` in the kind name table; an `Application` is a program route)
- Modify: `modules/buildmcpp/src/directives.cppm:887-970` (`windows_subsystem` and `windows_entry` accept a program, not only `Binary`)
- Modify: `src/build/execute.cppm` (the run path: when the selected target is an `Application` whose form on the resolved triple is `SharedObject` and no `--format` was given, refuse: `error: 'myapp' is an application, and on aarch64-linux-android an application is a shared library that a package installs. Run it through a distributable: mcpp run --format <name>, where <name> is one of: <the formats the graph provides, or 'none declared'>`)
- Test: `tests/unit/test_manifest_targets*.cpp` (find the file that asserts the `kind` refusal text), `tests/unit/test_artifact_naming.cpp` (the form function), new `tests/e2e/652_an_application_is_a_kind.sh`

**Interfaces:**
- Produces: `Target::Application`, `Target::is_program()`,
  `mcpp::toolchain::triple::application_form(const Triple&)`. T6 uses the refusal
  path and the form function; P4 names the target's file with
  `${mcpp.target_file:<name>}` and expects `lib<name>.so` on Android.

- [ ] Step 1: unit tests: `kind = "app"` parses to `Application`; the refusal for
  `kind = "x"` lists `'bin', 'app', 'lib' or 'shared'`; `application_form` is
  `SharedObject` for `aarch64-linux-android` and `x86_64-linux-android`, `Executable`
  for `x86_64-linux-gnu`, `aarch64-ios-sim`, `wasm32-emscripten`, `x86_64-windows-msvc`;
  `windows_subsystem = "windows"` is accepted on an `app` and refused on a `lib`.
- [ ] Step 2: run; expect failures.
- [ ] Step 3: implement the enum value, the parser, the descriptor reader, the form
  function, the `is_program()` sweep. For each of the 13 sites, decide in a one-line
  comment whether the question is "is a program" (use `is_program()`) or "is
  literally an executable link" (keep `Binary` and add `Application` with the form
  check). The output file name derives from the link form: `lib<name>.so` when the
  form is `SharedObject`.
- [ ] Step 4: run the unit tests; expect pass.
- [ ] Step 5: the run refusal in `execute.cppm`, with the format set taken from
  `plan.providedPackFormats` if the plan is available at that point, otherwise the
  literal `none declared`.
- [ ] Step 6: e2e `652` (`# requires: elf gcc`): a package with `[targets.myapp] kind = "app"`
  and `[targets.tool] kind = "bin"`. On the host: `mcpp build` links `bin/myapp` and
  `bin/tool`; `mcpp run --bin myapp` (or the selection flag the run command uses)
  prints its marker. With `--target aarch64-linux-android` and the NDK payload
  absent, the plan-level assertion is made through `mcpp build --dry-run` or `mcpp
  emit ninja` if either prints the link units without a toolchain; if neither does,
  gate the Android leg with `# requires: android-ndk` in a second script
  `652b_...` and assert there: `lib/libmyapp.so` is linked, `bin/tool` is linked
  (`bin` keeps meaning binary), `mcpp run` without `--format` is refused with the
  sentence naming `--format`, `mcpp test` builds and runs the test binary through
  the declared runner or is skipped by the existing rule.
- [ ] Step 7: assert the older-engine behaviour by reading, not by running: the
  refusal text at `toml.cppm:1110` on 2026.9.12.2 names three kinds; state it in the
  e2e's header comment.
- [ ] Step 8: run; expect pass. Full unit suite.
- [ ] Step 9: commit: `an application is a kind, and its link form is the row's (#622 A3)`.

## Wave 2

### T4: `abi.exceptions` and `requires_abi` on the target axis

**Files:**
- Modify: `modules/manifest/src/types.cppm` (beside `abiThreads`/`abiThreadsDeclared`: `abiExceptions`, `abiExceptionsDeclared`; `requiresAbiExceptions` beside `requiresAbiThreads`; `featureRequiresAbiExceptions`; per-selector requirement tables)
- Modify: `modules/manifest/src/toml.cppm:2752-2768` (accept `exceptions`; the refusal names `threads, exceptions`), `:949-957` and `:767-776` (parse `exceptions` in `requires_abi`), the `[target.<sel>]` body parser near `:2731` (accept `requires_abi` and `feature-requires-abi` under a selector; store with the selector for the merge pass)
- Modify: `src/build/prepare.cppm:2856-2879` (render `-fexceptions` on `os == "emscripten"` into dialect, cflags and ldflags via `add_once`, only for the root), `:7455-7466` (accumulate exception requirements; the dependency-table warning covers both members), `:8909-8922` (check each member; the refusal names the member and, for a selector-sourced requirement, the selector), `merge_conditional_config` (union the selector's `requires_abi` into the package's requirement set when the predicate matches)
- Test: `tests/unit/test_abi.cpp`; new `tests/e2e/653_exceptions_is_the_second_abi_member.sh` (`# requires: emsdk`, same gate as T1's), new `tests/e2e/654_a_requirement_on_the_target_axis.sh` (`# requires: elf gcc`)

**Interfaces:**
- Consumes: nothing from wave 1.
- Produces: `[target.<sel>.abi] exceptions`, `requires_abi = { exceptions = true }`,
  `[target.<sel>] requires_abi = {...}`, `[target.<sel>.feature-requires-abi] <f> = {...}`.

- [ ] Step 1: unit tests in `test_abi.cpp`: `exceptions = true` parses; `exceptions = 1`
  refused with "must be true or false"; `[target.x.abi] frobnicate` refused naming
  `threads, exceptions`; `[target.'cfg(linux)'] requires_abi = { threads = true }`
  parses into a selector-scoped requirement; `[target.'cfg(linux)'.feature-requires-abi] mt = { threads = true }` parses; an unknown member inside `requires_abi` is refused naming the two.
- [ ] Step 2: run; expect failures.
- [ ] Step 3: implement parsing and storage.
- [ ] Step 4: run; expect pass.
- [ ] Step 5: rendering and the check. On a Linux root with `exceptions = true`
  under `cfg(linux)`, nothing is rendered and the cache key is unchanged (the
  member is satisfied by default); on `cfg(os = "emscripten")` it is rendered. The
  selector-scoped requirement is evaluated against the resolved triple in the same
  pass as `[target.<sel>.build]`.
- [ ] Step 6: e2e `654`: a dependency with `[target.'cfg(linux)'] requires_abi = { threads = true }`
  and a root without the table: refused naming the dependency and `cfg(linux)`; with
  `[target.'cfg(linux)'.abi] threads = true`: builds; the same dependency under
  `--target x86_64-windows-msvc` if a cross toolchain is available is not required
  (or, without one, a unit test asserts the merge result for a Windows triple);
  the feature form: refused only when the feature is active.
- [ ] Step 7: e2e `653` on the emsdk lane: a Web program that throws and catches
  across an `import std` boundary; with `[target.'cfg(os = "emscripten")'.abi]
  exceptions = true` it runs under the payload's node and prints its marker; without
  it the link or run fails (record the exact failure line in the script's comment);
  `-fexceptions` appears on the std module prebuild command and on a dependency's
  compile command in `build.ninja`; the two builds land in different fingerprint
  directories; a Linux build of the same manifest has a byte-identical `build.ninja`
  with and without the table.
- [ ] Step 8: run both; expect pass. Full unit suite.
- [ ] Step 9: commit: `exceptions is the second abi member, and a requirement can sit on the target axis (#622 A1, A6)`.

### T5: `frameworks` under `[target.<sel>.runtime]`, and the `platforms` vocabulary

**Files:**
- Modify: `modules/manifest/src/toml.cppm:2787-2800` (`kKnownCondRuntimeKeys` gains `frameworks`; the per-target runtime block stores it), the merge that appends per-target `libraries` (find with `grep -n "link_library_dirs" src/build/prepare.cppm src/build/plan.cppm`) appends `frameworks` the same way
- Modify: `src/build/prepare.cppm:2256-2266` (the vocabulary becomes `linux | macos | windows | ios | android | emscripten`; the message lists six)
- Modify: `src/pack/library_pipeline.cppm:375-435` (`servable_here` asks `host_can_serve` per known row instead of a hard-coded triple list; the platform of a row is computed by one function `platform_name(const Triple&)` placed beside `artifact_naming` in `triple.cppm`: `env == "android"` gives `android`, else `os`)
- Modify: `src/doctor.cppm:1032-1039` (prints the six-word vocabulary in its hint if it prints one)
- Test: `tests/unit/test_target_runtime*.cpp` or the file that asserts `kKnownCondRuntimeKeys` (`grep -rl "link_library_dirs" tests/unit`), `tests/unit/test_artifact_naming.cpp` (`platform_name`), new `tests/e2e/655_frameworks_are_per_target.sh` (`# requires: macos` if such a gate exists, else a unit test on the rendered link line for a Mach-O triple through the flags module), an assertion added to the existing `platforms` e2e (`grep -l "unknown platform" tests/e2e/*.sh`)

**Interfaces:**
- Produces: `[target.<sel>.runtime] frameworks`, `platform_name(const Triple&)`.

- [ ] Step 1: unit tests: `[target.macos.runtime] frameworks = ["AppKit"]` parses and is
  no longer warned; the rendered Mach-O link line for `aarch64-macos` carries
  `-framework AppKit` after the top-level list; for `aarch64-ios-sim` with
  `[target.'cfg(os = "ios")'.runtime] frameworks = ["UIKit"]` it carries `UIKit` and
  not `AppKit`; for `x86_64-linux-gnu` neither; `platform_name` for the six rows;
  `platforms = ["emscripten"]` passes and `["web"]` is refused under `--strict`
  naming the six.
- [ ] Step 2: run; expect failures.
- [ ] Step 3: implement.
- [ ] Step 4: run; expect pass.
- [ ] Step 5: e2e assertion for `platforms` in the existing script; a Linux e2e that
  a manifest with both `frameworks` tables builds byte-identically to one without
  (`build.ninja` compared).
- [ ] Step 6: commit: `frameworks is per target, and platforms names the rows that exist (#622 A2, A7)`.

### T6: `mcpp run --format <name>`

**Files:**
- Modify: `src/cli.cppm:420-500` (a `format` option on `run`; refused together with `no-runner`)
- Modify: `src/cli/cmd_build.cppm` and `src/build/execute.cppm` (when `--format` is given: run the pack pipeline exactly as `mcpp pack --format <name>` does, obtain the artifact path it reports, then enter the ordinary run path with that path as the operand; the runner resolution is unchanged)
- Modify: `src/pack/*` (the pack entry returns the reported artifact path in its result rather than only printing it; find the print with `grep -rn "reports the artifact\|introduced" src/pack/*.cppm`)
- Test: new `tests/e2e/656_run_hands_the_distributable_to_the_runner.sh` (`# requires: elf gcc`)

**Interfaces:**
- Consumes: T3's `application_form` for the refusal message set; the pack pipeline's report.
- Produces: `mcpp run --format <name>`.

- [ ] Step 1: e2e `656`: a package whose build program declares
  `provides_pack_format("blob")` and, when `pack_format() == "blob"`, submits an
  artifact action that copies `${mcpp.target_file:app}` to `${mcpp.out_dir}/app.blob`;
  `[target.<host triple>] runner = ["<fixture script>"]` where the script prints
  `RUNNER: <operand>` and executes the operand if it is executable, else copies it
  back to a path and runs that. Assert: `mcpp run --format blob` prints
  `RUNNER: .../app.blob` and the program's marker; `mcpp run` alone prints
  `RUNNER: .../bin/app`; `mcpp run --format bogus` is refused naming `blob`;
  `mcpp run --format blob --no-runner` is refused; `mcpp build` afterwards has a
  plain graph (`is_plain_build_graph`, assert through the `dist=none` header line
  of `build.ninja`).
- [ ] Step 2: run; expect failure at the unknown option.
- [ ] Step 3: implement the option, the pipeline call, the operand substitution, the
  two refusals.
- [ ] Step 4: run; expect pass. Also run `tests/e2e/6xx` scripts that cover `mcpp pack
  --format` (`grep -l "pack --format" tests/e2e/*.sh`) to show the pack path is
  unchanged.
- [ ] Step 5: commit: `mcpp run --format hands the distributable to the runner (#622 A10)`.

## Wave 3

### T7: documentation, CHANGELOG, the record's status

**Files:**
- Modify: `docs/04-mcpp-toml.md` (§2.2 `kind = "app"` with the form table; `[targets.<name>]` key list unchanged; §2.11 `frameworks` per target; §2.12 the six platform names), `docs/10-pack-and-release.md` (`run --format`; the wasm family in the layout table), `docs/21-the-target-triple.md` (a paragraph "The wasm artifact contract" after the object-format table; `application_form` in the Android row's notes), `docs/22-target-side.md` (`abi` table with two members; `requires_abi` on the target axis; `frameworks` in the per-target runtime list; `--no-entry` note), `docs/30-build-mcpp.md` (directive `deploy`, protocol 11, the environment row `MCPP_TARGET_MIN_PLATFORM_VERSION`, `min_platform_version()`), `docs/31-authoring-a-rule-package.md:341-346` (the second sentence of "Declare the tool where it will be looked up"), `docs/zh/` mirrors of each, `CHANGELOG.md` (`[Unreleased]` entries per item, in the existing register), the design record's status line (`proposal, for review` becomes `implemented in mcpp <version>; ecosystem changes follow`)
- Check: `bash .github/tools/check_docs_style.sh && bash .github/tools/check_docs_structure.sh && python3 .github/tools/gen_agents_index.py`

- [ ] Step 1: write each section, one commit per doc pair (English and zh) or one
  commit for all, in the register of the neighbouring text.
- [ ] Step 2: run the three checks; expect `OK` lines.
- [ ] Step 3: commit: `docs: the keys, the row and the directive of #622`.

## Wave 4

### T8: version, CI, self-review, merge

- [ ] Step 1: `git fetch origin main`; rebase the feature branch; the three hygiene
  commands from Global constraints.
- [ ] Step 2: bump `mcpp.toml [package].version` and `src/version.cppm` to the
  release date's version (`YYYY.M.D.N`, `N` starting at 1) in one commit;
  `bash .github/tools/check_version_pins.sh`.
- [ ] Step 3: open the PR with a body that lists each item with its criterion and
  the e2e that holds it; wait for every workflow; read a failing job's log before
  changing anything.
- [ ] Step 4: self-review against the spec: each §2 item has a task, each task's
  negative direction is in a test, no doc names a key by a spelling the parser does
  not accept (grep each new key in `docs/` and in `modules/manifest/src/toml.cppm`).
- [ ] Step 5: merge (squash), confirm `origin/main` HEAD is the squash and the
  workflows on that SHA are green.

## Wave 5: the ecosystem

### X1 to X5: `openxlings/xim-pkgindex`

One PR: `pkgs/a/android-build-tools.lua` (Google's `build-tools_r<v>-<host>.zip`,
`aapt2`, `zipalign`, `apksigner`, `d8` registered; `NOTICE` read and the tier
recorded in the header as `android-ndk.lua` does), `pkgs/a/android-platform-<api>.lua`
or one `android-platform.lua` versioned by API level (`android.jar`; the same
reading), `pkgs/a/android-platform-tools.lua` gains `adb-run` (a bash script in
`bin/`, the two operand shapes of §4.2, `bash -n` at install),
`pkgs/a/apple-simulator-tools.lua` gains the `.app` branch of §4.3,
`pkgs/a/android-debug-keystore.lua` (a `debug.keystore` generated once with
`keytool -genkeypair -alias androiddebugkey -storepass android -keypass android
-dname "CN=Android Debug,O=Android,C=US" -validity 10000`, checked into
`xlings-res` as an asset with its sha256, installed to `<root>/debug.keystore`).
Each recipe's criterion is the sandbox: `xlings install <pkg>` then the program
answers `--version` or the file's sha256 matches.

### P1 to P5: `mcpp-community/mcpp-plugins`

One PR: `dist/wix.cppm` payload tier only and the README/manifest correction
(§3.5); `dist/apple.cppm` iOS branch (§3.1) reading `mcpp::min_platform_version()`;
new `dist/web.cppm` (§3.3); new `dist/apk.cppm` level 0 then level 1 (§3.2), with
its payloads under `[target.'cfg(env = "android")'.feature-xlings.dist-apk]`;
fixtures `tests/web-consumer`, `tests/apk-consumer`, `tests/ios-app-consumer`; CI
`MCPP_VERSION` raised to the released engine; version bumped; release.

### I1: `mcpplibs/mcpp-index`

`mcpp:plugins` at the new version, published after the plugins release; the index
CI pin raised to the engine that carries the directive (the floor stays).

### Release and verification

Follow `.agents/skills/mcpp-release/SKILL.md`. After the release: `xlings subos
use <name> --sandbox`, `mcpp self config --mirror CN` inside it, then the
verification script `.agents/docs/2026-09-12-622-verify.sh` covering: the wasm
family and `run` on the Web row; `deploy` from a build program; `kind = "app"` on
the host and the Android refusal; `requires_abi` on the target axis; `frameworks`
per target (plan-level on Linux); `platforms = ["emscripten"]`; `run --format`
with the stub runner; then, through the index, `mcpp:plugins`' `dist-web` on the
Web program and `dist-apk` level 0 on `x86_64-linux-android` with the emulator
lane if the sandbox host can run it. Each section clears its own probe directory.
