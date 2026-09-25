---
subject: design
status: landed
---

# Workspace inheritance, flag scoping and the published form: a unified repair plan (#690)

- Issue: mcpp-community/mcpp#690 (2026-09-24), "`[workspace.build] defines` are lost for sibling path dependencies"
- Basis: `origin/main` b4824697 (mcpp 2026.9.24.1). Every measurement was taken on Linux x86_64 with gcc 16.1.0, using a binary built from that commit. The F7 A/B runs used the same commit with one environment-gated change, described in section 3.7.
- Status: plan awaiting review; nothing is implemented. Decision D1 (section 4) was accepted in review on 2026-09-25.

---

## 0. Summary

1. **#690 is a defect in mcpp; the reported usage is correct.** `docs/07-workspace.md` §4.1, the design record of #527 (D12), and the CHANGELOG entry of #538/#539 all promise that `[workspace.build]`, including `defines`, reaches a member that is compiled as a sibling's `path` dependency. The defect does not depend on the platform.

2. **The audit found nine findings in three families.** None of them is an isolated slip:
   - *Inheritance placement* (F1, F2, F8). #539 applied `[workspace.build]` while `makePackageRoot` captures the manifest into the build graph, instead of in the normalisation pipeline that runs before that capture. A dependency member therefore inherits after its `defines` have been folded (F1), and the root inherits twice (F2).
   - *The effective manifest exists only inside `prepare_build`* (F3, F4, F5, F6). Other readers see the raw file or a partial merge: the parser's key table, git-hosted members, `publish`/`pack`/`emit xpkg`, and `toolchain list`. The published form of a member is not self-contained. It loses `[workspace.build]`, keeps sibling `path` edges that consumers cannot resolve, and omits those edges from the descriptor.
   - *Flag scoping* (F7, F9). The root's include directories, including `private_include_dirs`, are broadcast to every dependency's translation units. The dependency cache key does not contain them, so a root's private header can change a dependency object that is then served to unrelated projects. Measured: project B received a cJSON object compiled against project A's private `float.h`. Separately, the documented override rule for inherited vectors ("the member's later flag wins") holds only for flags the compiler resolves last-wins.

3. **The plan has seven workstreams and three structural rules.** Every member passes through one pipeline. Every reader of a member manifest reads one effective manifest. Private build requirements never cross a package boundary. The workstreams are ordered so that each lands with its own criterion (section 7).

---

## 1. Method

- Each finding is either *measured* (a fixture and a command whose output is quoted) or *reasoned* (a code path is cited). The distinction is stated for every finding.
- Criteria assert on `compile_commands.json`, link lines, program output and file contents, never on build success alone (design record of #527, section 5.6).
- F7 was measured as an A/B with one binary: `src/build/flags.cppm` gated the root include broadcast behind `MCPP_F7_NO_BROADCAST`, so both modes ran the same build of the same commit.
- A pre-existing corruption in this machine's `~/.mcpp/registry/data/xpkgs/xim-x-glibc/2.44.3` affects `tests/e2e/31_transitive_deps.sh` in both modes. Seven files in that directory, including the `libc.so` and `libm.so` linker scripts, reference a deleted temporary registry (`/tmp/tmp.Iq9cIYV03q/h2/...`) and were rewritten on 2026-09-24 04:28, before this work began. This is the #293 write-through shape. Test 31 therefore carries no signal about F7.

---

## 2. Principles

The decisions below are derived from these rules. Each rule names its source in mcpp and its counterpart in established build systems.

| | Rule | In mcpp | Elsewhere |
|---|---|---|---|
| **P1** | **Position independence.** A package's compile inputs are a function of the package and of graph-wide settings. They do not depend on which consumer reached it, on whether it is the root, or on how it was fetched (path, git, index). | `docs/04`: "mcpp deliberately does not compile a shared source two different ways within one build"; #359: a package's own statements "mean the same thing whether the package is the root or someone's dependency". | Cargo resolves `field.workspace = true` at manifest load for every member, including a git dependency whose repository is a workspace. |
| **P2** | **Merge, then normalise, then snapshot.** Derived fields are computed once, after every merge. Consumers read only the normalised snapshot, and the snapshot checks the pipeline's post-condition. | #229 comment on `merge_conditional_config`: merges run "always immediately BEFORE" the capture; the comment on `fold_build_defines_into_flags` states the same order. | General practice for configuration pipelines. A merge after normalisation creates a second, partially normalised state. |
| **P3** | **One source of truth per rule.** One merge function, one key table, one effective-manifest loader. | D12: "a fifth key added to one of them is a defect that compiles". | |
| **P4** | **Scope.** A package's private build requirements reach only its own units. Usage requirements flow from a dependency towards its consumers, never from a consumer into a dependency. | #101 (usage-requirements architecture): the root's `include_dirs` is its private build requirement. `docs/04`: `private_include_dirs` "stop at this package's own boundary". `docs/30`: `include_dir` "colours only the declaring package's own translation units". | CMake `PRIVATE`/`PUBLIC`/`INTERFACE`. Bazel `copts` are not inherited by dependencies. |
| **P5** | **Cache soundness.** Every input that reaches a cached command is part of its key. | Memory of #344 and the dependency BMI poisoning: "the generated artefact is the criterion; the source is the hypothesis". | ccache and Bazel key on the full command and the content of its inputs. |
| **P6** | **The published form is self-contained and equals the development form.** | `docs/11`; memory "published form differs from development form". | `cargo package` writes a normalised `Cargo.toml` (inherited fields inlined, `path` removed from dependencies, original kept as `Cargo.toml.orig`). pnpm rewrites `workspace:` ranges at publish. |
| **P7** | **Internal invariants fail loudly in release builds.** | `plan.cppm:1797` `"internal error: ... (please report)"` returned through `std::expected`. There is no `assert` in `src/`. | |
| **P8** | **Behaviour changes carry a measured blast radius.** A change that can turn green builds red is measured against the ecosystem first, and is degraded first when the condition is not a proven failure. | D13 in the #527 design record; the ecosystem CI with `MCPP_SOURCE_REF`. | |

---

## 3. Findings

### 3.1 Every inheritable key in every position

`[workspace.build]` accepts fifteen keys (`modules/manifest/src/toml.cppm:4002-4050`). Counts are occurrences in the member's own translation units. The fixture declares every vector key in the workspace, and a member-own `defines`, `cxxflags` and `cflags` as the control.

| Key | Read from | Root (`-p lib`) | Sibling (`-p app`) | Finding |
|---|---|---|---|---|
| `defines` | `privateBuild` after the fold | 1 | **0** | **F1** |
| `cxxflags` | `privateBuild.cxxflags` | **2** | 1 | **F2** |
| `cflags` (`.c` unit) | `privateBuild.cflags` | **2** | 1 | **F2** |
| member's own `defines`/`cxxflags`/`cflags` (control) | same | 1 | 1 | correct |
| `ldflags` | root link line | **2** | n/a | **F2** |
| `include_dirs`, `include_dirs_after` | `privateBuild` plus the root broadcast | 2 | 2 | see F7 |
| `dialect_cxxflags`, `c_standard`, `linkage`, `target`, `cxx_runtime`, `dependency_linkage`, `macos_deployment_target` | root manifest only | graph-wide | n/a | consistent |
| `ios_deployment_target` | parsed and inherited | **refused** | n/a | **F3** |

**Correction (2026-09-26, mcpp#695).** The row above classifies `c_standard` as read from the root manifest only and calls that consistent. For C the classification is wrong. A C translation unit produces no BMI, so nothing requires one C standard across the graph, and the package's own cache key and fingerprint already recorded the value as the package's. The root's value reached every dependency's C units through the file-level `$cflags` line, and a dependency's own declaration was not applied. From mcpp 2026.9.26.1 each package's C units compile at that package's own standard (docs/04, "`c_standard` applies to the package that declares it"); the plan is `2026-09-25-issues-693-696-triage-and-repair-plan.md`.

### 3.2 Inheritance placement: F1, F2, F8

**F1 (measured): `defines` are lost for a member reached as a `path` dependency.** On the dependency branch, `fold_build_defines_into_flags(dep_manifest->buildConfig)` (`src/build/prepare.cppm:8235`) folds and clears `defines`. `makePackageRoot` (`prepare.cppm:6709`) then calls `inherit_workspace_build`, which prepends the workspace `defines` to the folded manifest. It copies `cflags`/`cxxflags` into `privateBuild` without folding again. The failure is silent unless the source guards the macro. Workspace-wide defines are typically layout- or ABI-affecting (`_ITERATOR_DEBUG_LEVEL`, `_WIN32_WINNT`, `UNICODE`, `FMT_HEADER_ONLY`, `SPDLOG_ACTIVE_LEVEL`), so a member compiled without them is an ODR violation against members compiled with them.

**F2 (measured): the selected root member inherits twice.** The root inherits at load (`prepare.cppm:2365`/`2384`). `packages[0] = makePackageRoot(*root, *m)` (`prepare.cppm:6774`) finds it is a member and inherits again. The measured compile line is `-DFLAGLEVEL=1 -fno-exceptions -DFLAGLEVEL=1 -fno-exceptions -DFLAGLEVEL=2 -fexceptions`, which is the workspace entries twice followed by the member's once. The second copy is prepended as well, so the outcome of any override is unchanged. The cost falls on flags that are not idempotent, such as `-include x.h` without a guard, or options that accumulate.

**F10 (measured during implementation): a member reached as a dependency does not resolve its own `x.workspace = true` entries.** A sibling `lib` whose manifest says `util.workspace = true` builds under `-p lib`. Under `-p app` it fails with `dependency 'util' has SemVer constraint '' but no readable index entry for it`. The dependency load site applied only `inherit_workspace_package`, so the entry reached resolution with neither version nor path. This is the same placement defect as F1, for the third part of what a member inherits.

**F11 (measured during implementation): a member inside an index package's archive does not inherit its archive's workspace.** A descriptor may point at a member manifest (`mcpp = "*/mcpp/cairo/mcpp.toml"`; 22 installed index packages on the measuring machine have a workspace root in their archive). The resolver loaded that manifest as a stand-alone file, so a member that omits `version` was refused and `[workspace.build]` was ignored, while the same commit consumed through `git` inherits (D1). None of the 22 installed archives declares `[workspace.package]`, `[workspace.build]` or `[workspace.dependencies]`, so applying the inheritance changes no existing package. It is applied through the same function as the sibling and git cases, searching no higher than the install root (e2e 774).

**F12 (measured during review): a dependency built as a host tool merges its conditional sections twice.** The resolver merges a dependency's `[target.<selector>.build]` sections for the consumer's target, and the host-tool sub-build received that merged manifest and merged it again for the host. A package whose matching section adds `-include once.h` (a header without a guard) builds on its own and fails as a host tool with `redefinition of 'int once_counter'`, on 2026.9.24.1 as well. The comment above the sub-build called the manifest "pristine". The manifest now records its state before the first merge (`Manifest::beforeConditionalMerge`), and the sub-build receives that state (e2e 775).

**F8 (reasoned): the F1 dependency's cache key records a define its compile does not carry.** `cache_key.cppm:534` reads `pkg.manifest.buildConfig.defines`, which retains the unfolded entries.

Root cause: the build half of inheritance runs inside the snapshot, after the fold.

```
root:        load -> inherit(package+build) -> cfg merge -> report -> fold -> snapshot
path member: load -> inherit(package)       -> cfg merge -> report -> fold -> snapshot+inherit(build)   (F1)
root again:                                                                   snapshot+inherit(build)   (F2)
```

### 3.3 The key table: F3

**F3 (measured): `[workspace.build] ios_deployment_target` is refused although it is parsed and inherited.** #612 (c58d61e6) added the assignment (`toml.cppm:4025`) and the inheritance (`src/project.cppm:284`), but not the `kKnown` entry (`toml.cppm:4027`) or the error text. The key set is written four times: assignments, `kKnown`, the error message, and `docs/07`.

### 3.4 Git-hosted members: F4

**F4 (measured): a member of a git-hosted workspace inherits `[workspace.package]` but not `[workspace.build]`.** The repository builds its member with `mcpp build -p lib`. A consumer of the same commit through `{ git = "file://...", branch = "main" }` fails with `#error WS_FLAG missing`. The package half was added in #650 (`prepare.cppm:8210`). The build half is excluded by a comment in `makePackageRoot` that considers only the consumer's workspace.

### 3.5 Readers outside `prepare_build`: F5 (load half), F6

**F5a (measured): `mcpp publish`, `mcpp pack` and `mcpp emit xpkg` reject a member that omits `version`.** `docs/07` §4.1 permits the omission. `publish/pipeline.cppm:29,86` and `pack/route.cppm:64` call `manifest::load` without `insideWorkspace` and without inheritance.

**F6 (measured): `mcpp toolchain list` inside a member ignores the workspace's `[toolchain]`.** It marks `gcc 16.1.0 (default)` while `mcpp build` in the same directory resolves the workspace's `llvm@22.1.8`. `effective_default_toolchain` (`toolchain/lifecycle.cppm:412`) reads `./mcpp.toml` raw.

### 3.6 The published form: F5 (publish half)

Measured on a git repository whose workspace declares `version`, `license` and `cxxflags = ["-DWS_FLAG=1"]`, with a member `lib` that depends on a sibling `util` through `path = "../util"`:

| Observation | Evidence |
|---|---|
| **F5b.** The source archive contains only the member directory. | `git -C <member> archive HEAD` (`src/pm/publisher.cppm:374`). The listing is `lib-0.3.0/lib.cpp` and `lib-0.3.0/mcpp.toml`. |
| **F5c.** The archived `mcpp.toml` is the raw member file. | It still reads `"probe.util" = { path = "../util" }`. It carries neither `[workspace.package]` nor `[workspace.build]`. |
| **F5d.** The descriptor points consumers at that raw file. | `manifest = "mcpp.toml"` (`publisher.cppm:329`). `loadVersionDep` reads it from the installed archive. |
| **F5e.** The descriptor drops sibling edges silently, even when they carry a version. | `deps = {}`. `publisher.cppm:297` skips every `isPath()` edge. An edge written `{ path = "../lib", version = "0.1.0" }` is accepted by the manifest and builds, but it is still omitted. |
| **F5f.** A consumer of the archive cannot build it. | Depending on the unpacked archive (standing in for an index consumer, which reads the same file) fails with `path dependency 'probe.util' ... has no mcpp.toml`. With the sibling edge removed by hand, it fails with `#error WS_FLAG missing: the published form lost [workspace.build]`. |

`mcpp pack` builds through `prepare_build`, so its binary artefacts carry the effective configuration once F5a is fixed (reasoned from `pack/library_pipeline.cppm:303`, which reads the prepared context's manifest).

### 3.7 Flag scoping: F7

**F7a (measured): the root's `include_dirs`, including `private_include_dirs`, reach every dependency's units.** `flags.cppm:612-624` places `plan.manifest.buildConfig.includeDirs` and `includeDirsAfter` in the file-level `$cxxflags`/`$cflags` of `build.ninja`. Every rule reads those variables (`cxx_object`, `c_object`, `cxx_module`, `cxx_scan`), in addition to each unit's `$local_includes`. This broadcast predates the usage-requirements model (v0.0.1). #101 introduced `privateBuild` for the root but did not remove it. The NASM channel (`flags.cppm:1128`) has the same shape.

- Path dependency: a root with `include_dirs = ["appinc", "appprivinc"]` and `private_include_dirs = ["appprivinc"]` puts both directories on the dependency's `lib.cpp` and `c.c`. The root's own units carry each directory twice.
- Index dependency (`compat.cjson` 1.7.19): a root `appprivinc/limits.h` containing `#error` stops `cJSON.c` from compiling (`In file included from .../cJSON.c:44`), with `--cache off`. The comment in `mcpp-index/pkgs/c/compat.godot-cpp.lua:163`, "a consumer-side header shadow never reaches it", does not hold for an uncached compile.

**F7b (measured): the dependency cache key omits the broadcast, so the shadow crosses projects.** Two projects that differ in root `include_dirs` resolve the same key (`build-cache/v1/pkg/compat/compat.cjson@1.7.19/7bc4625c67943f6a`). The procedure was as follows. The cache entry was moved aside. Project A, whose root has a private `float.h` that redefines `DBL_EPSILON` to `0.5`, was built. Then project B, which has no include directories, was built. B reported `Cached compat.cjson` and printed `cJSON_Compare(1.0, 1.2) = 1`. With the original entry restored, B prints `0`. The object a project receives therefore depends on which project populated the cache first. This is the most severe finding in this record, because it is silent, it crosses project boundaries, and it survives until the cache entry is evicted.

**Blast radius of removing the broadcast (measured).** With `MCPP_F7_NO_BROADCAST=1`:
- The root's own units receive each root directory exactly once. Dependency units receive none. A dependency's public directories still reach its consumer (`lib/inc` on `main.cpp`).
- A root `build.mcpp` that calls `mcpp::include_dir("gen")` still reaches the root's units.
- The twenty e2e scripts that exercise `include_dirs` give identical results in both modes: every one passes, except `31_transitive_deps`, which fails in both modes for the reason given in section 1. No script relies on the broadcast.
- No descriptor in `mcpp-index` states that a package expects a header from its consumer's include path (searched for the usual phrasings).

### 3.8 Override semantics: F9

**F9 (measured): the documented override rule for inherited vectors holds only for last-wins flags.** `docs/07` §4.1 says a member's flag "comes later on the command line, where it wins". The measured program output is `LEVEL=2 FLAGLEVEL=2 header=workspace exceptions=1`:
- `-f`/`-fno-`, `-W`/`-Wno-`, `-O` and a redefined `-D` are overridden as documented.
- **Include directories are first-wins.** The workspace's `wsinc/pick.h` shadows the member's `lib/inc/pick.h`.
- **A redefined macro is a diagnostic.** `-DLEVEL=1 -DLEVEL=2` produces `warning: 'LEVEL' redefined`. mcpp does not show it on a successful build, and it is an error under `-Werror`.
- **A workspace define cannot be removed.** The fold appends every `defines` entry after all `cxxflags`, so a member's `-ULEVEL` precedes the workspace's `-DLEVEL=1`. Measured: `LEVEL` stays defined.

### 3.9 Checked and consistent

- The fast path with an inherited `target` produces the correct `Target` for each change in the workspace and in the member (measured).
- Graph-wide keys are read from the root manifest only (`flags.cppm:1038`, `prepare_inputs.cppm:568`, `prepare.cppm:11911`).
- The multi-version and SemVer-merge `makePackageRoot` sites (`prepare.cppm:7786`, `7886`) receive index packages from `loadVersionDep`, which are never members.
- `mcpp doctor` reads only non-inheritable keys.
- Non-member `path` dependencies acquire no workspace flags (e2e 321, and observed here).
- The root's `cxxflags`/`cflags` are not broadcast: a dependency unit carries its own flags only (measured). Only include directories are.

---

## 4. Decisions

| | Question | Decision | Basis |
|---|---|---|---|
| **D1** | Should a git-hosted workspace's `[workspace.build]` reach its git-consumed members? | **Yes.** Accepted in review. | P1. The flags are the member's statement about its own compilation, factored into its repository root. A member's own `[build]` already applies through git, and #650 adopted the package half for the same reason. The behaviour change is recorded in the CHANGELOG. |
| **D2** | What is the published form of a member? | **A normalised, self-contained manifest** (section 5.5). Publishing is refused only where normalisation cannot preserve meaning. | P6. The development form is valid only inside its workspace. Refusing every member whose effective manifest differs from its file would refuse every member that inherits anything, which is the purpose of a workspace. |
| **Q3** | Is the snapshot post-condition an internal error in release builds? | **Yes**, following `plan.cppm:1797`: `std::unexpected("internal error: ... (please report)")`. | P7. The defect it catches is silent, and the check costs one comparison per package. |
| **D3** | How does a member override inherited vectors? | **(a)** document the actual rule; **(b)** make `defines` a keyed set. Both are delivered in the #690 pull request (review of 2026-09-25 asked for one pull request per repository). Include ordering and opt-out wait for evidence. | Section 5.7. |
| **D4** | What replaces the root include broadcast? | **Nothing implicit.** The root's include directories become private to the root, as #101 intended. A dependency that needs a consumer-supplied header receives it through an explicit, keyed mechanism, designed only when a package needs it. | P4, P5, and the measured absence of reliance. |

---

## 5. Design

### 5.1 W1: one pipeline for every member (F1, F2, F8)

- In the path/git dependency branch (`prepare.cppm:8199-8209`), call `inherit_workspace_build` directly after `inherit_workspace_package`, under the same membership condition. This places it before `merge_conditional_config`, `report_flag_words_changes` and the fold, exactly as on the root.
- Remove the inheritance block from `makePackageRoot`, and take the manifest by `const&` again. The root is inherited once, at load.
- At the snapshot, check the post-condition `manifest.buildConfig.defines.empty()`. On violation, return `internal error: [build].defines reached the build graph unfolded for '<package>' (please report)`.
- Rewrite the #539 comment in `makePackageRoot` so that it records why inheritance is not performed there.

Alternatives rejected: folding again inside `makePackageRoot` fixes F1 but leaves F2, and it leaves inherited flags unreported by `report_flag_words_changes`. A special case that skips the root fixes F2 only, by adding the positional branch that P1 excludes.

### 5.2 W2: one key table (F3)

Replace the assignments, `kKnown` and the error text in `toml.cppm` with one table of rows `{ key, assign }`. Both the known-key check and the message are derived from the table. Add the row `ios_deployment_target`. `docs/07` refers to the keys instead of listing them.

### 5.3 W3: git-hosted members (F4, D1)

At the same load site, in the `gitMember` branch that already applies `inherit_workspace_package`, also apply `inherit_workspace_build(*dep_manifest, *rm, gitMemberCloneRoot)`. The repository root is the anchor for relative include directories. `docs/07` §6 states that both halves apply.

### 5.4 W4: one effective-manifest loader (F5a, F6)

Add `mcpp::project::load_effective_manifest(dir)`, which performs these steps:
1. Load `dir/mcpp.toml`.
2. Find the workspace root and decide membership with `is_workspace_member`.
3. Reload with `insideWorkspace` when the package is a member, and apply `inherit_workspace_config`.
4. Run `workspace_inheritance_error`.

The member branch of `prepare_build` (`prepare.cppm:2373-2387`), `publish/pipeline.cppm`, `pack/route.cppm` and `toolchain/lifecycle.cppm` call it. Every other raw `manifest::load` of a project manifest is reviewed against it. The review list is in `src/` (`cli/cmd_build.cppm:49`, `cli/cmd_sbom.cppm:94`, `pm/commands.cppm:572`, `pm/index_management.cppm:98,174`, `doctor.cppm:706`, `build/execute.cppm:1325`, `build/test_targets.cppm:38`). Sites that read only non-inheritable keys stay as they are, and each is recorded as such.

### 5.5 W5: the published form (F5b to F5f, D2)

`mcpp publish` and `emit xpkg` produce the archive from a normalised manifest:

1. **Inline the effective configuration.** Workspace-inherited `[package]` fields and `[workspace.build]` entries are written into the archived `mcpp.toml`. Vectors keep their inherited order, and scalars appear only where the member did not declare them. `x.workspace = true` dependencies are written with their resolved specification.
2. **Rewrite sibling edges.** A `path` edge to a workspace member is published as a version edge, using the `version` written on the edge. If the edge has no `version`, publishing is refused with a message that names the sibling's effective version and the line to add. The version is not inferred silently, because a bare version pin in mcpp is exact, and the constraint is the author's decision. A `path` edge to a non-member is refused, because the consumer cannot resolve it.
3. **Refuse what normalisation cannot carry.** An inherited include directory that resolves outside the member directory cannot exist in the archive, so publishing is refused and the directory is named.
4. **Emit the descriptor's `deps` from the normalised manifest**, so sibling edges appear there instead of being skipped.
5. **Keep the original** as `mcpp.toml.orig` in the archive, for audit, as Cargo does.
6. **Make it reviewable.** `mcpp publish --dry-run` writes the normalised manifest to `target/dist/` and prints its path.

This changes what consumers read only for members of workspaces. A package that is not a workspace member normalises to its own file.

### 5.6 W6: flag scoping (F7a, F7b, D4)

- Remove the root include broadcast from the C, C++ and NASM channels of `flags.cppm`. The root's units receive their directories through `privateBuild`, which is measured to be complete, including directories declared by `build.mcpp`.
- Make cache soundness a stated property: a dependency's compile command must be identical across consumers that differ only in root-private settings. A unit test compares the dependency's command for two roots that differ in `include_dirs`, `private_include_dirs` and `include_dirs_after`. It would have caught F7b.
- **Rollout (P8).** The twenty in-repository scripts show no reliance on the broadcast, but the ecosystem has not been measured. Before merge, run the mcpp-index CI against the pull-request branch (`MCPP_SOURCE_REF`). For one release, when a dependency's compile fails with a missing header that exists in a root include directory, append a note that names the directory and states that consumer include paths no longer reach dependencies. Cached objects produced under the broadcast are not invalidated by a key change, so the cache layout version is incremented with this change.
- An explicit consumer-to-dependency header channel (the "configuration header" pattern used by FreeRTOS, lwIP and mbed TLS) is out of scope. If a package needs it, it is designed per dependency edge, and it enters that dependency's cache key.

### 5.7 W7: override semantics (F9, D3)

- **(a) Now, in the W1 pull request:** `docs/07` states the rule as it is. Last-wins flags are overridden by restating them. Include directories are not overridable. A redefined define produces a compiler diagnostic, and a define cannot be removed.
- **(b) Follow-up:** `defines` becomes a keyed set. A member entry `NAME=value` replaces the workspace entry with the same `NAME`, so one `-DNAME=...` word is emitted and no redefinition diagnostic arises. An entry `!NAME` removes the inherited entry. This extends to vectors the rule scalars already follow: the member wins where it declared. It needs its own criterion and a SPEC-004 §8 amendment, because `defines` entries are currently values without identity.
- Member-first include ordering and an inheritance opt-out are not adopted. CMake orders directory-level before target-level include directories, as mcpp does. An opt-out weakens the drift protection that implicit inheritance exists to provide. Both are reconsidered only with a concrete case.

### 5.8 The rule, stated normatively

Proposed as SPEC-004 §9, "Workspace inheritance and build-requirement scope":

1. A workspace member **must** receive `[workspace.package]` and `[workspace.build]` exactly once, whether it is the selected root, a `path` dependency of another member, or a member of a git-hosted workspace.
2. Vectors are ordered workspace, then member, then matching `[target.<selector>.build]`. Scalars are taken from the workspace only when the member did not declare the key.
3. Inheritance **must** complete before `defines` is folded and before the manifest is captured into the build graph.
4. Every command that reads a member's manifest **must** read the effective manifest.
5. A package's private build requirements, including its include directories, **must not** reach another package's units.
6. The published manifest of a member **must** be self-contained.

---

## 6. Criteria

Each criterion fails when its fix is removed.

1. **W1, both positions and every per-package key exactly once.** e2e 321 is extended with a workspace that declares `defines`, `cflags`, `cxxflags` and `ldflags`, and a member that declares its own. Under `-p lib` and `-p app`, each workspace word occurs exactly once in the member's `.cpp` and `.c` entries and precedes the member's word. A C and a C++ `#error` guard make the denominator cover both channels. The negative leg (a non-member path dependency receives nothing) is retained.
2. **W1, post-condition.** A unit test passes a manifest with unfolded `defines` to the snapshot and receives the internal error.
3. **W2.** A unit test parses a workspace declaring every row of the key table, including `ios_deployment_target`, and asserts each inherited value.
4. **W3.** A `file://` repository fixture: the consumer build succeeds past an `#error` guard, and the flag occurs once.
5. **W4.** In a member that omits `version`, `emit xpkg` and `publish --dry-run --allow-dirty` succeed. In a member without `[toolchain]`, `toolchain list` marks the workspace's toolchain. Each has a control at the workspace root.
6. **W5.** From the section 3.6 fixture: the archived `mcpp.toml` contains the inherited `version`, `license` and `cxxflags` and a version edge to `util`. The descriptor's `deps` lists `util`. A consumer of the unpacked archive builds past the `#error` guard. An edge without `version` is refused with the sibling's version in the message. `mcpp.toml.orig` is present.
7. **W6.** (i) A root with a private `limits.h` containing `#error` builds a dependency, with `--cache off`, as a path and as an index dependency. (ii) The unit test of section 5.6: a dependency's compile command is identical under two roots that differ in include settings. (iii) The root's units carry each root directory exactly once. (iv) The ecosystem CI is green on the branch.
8. **W7(a).** The documentation change carries no criterion of its own. W7(b) receives one when it is designed.

---

## 7. Delivery

Superseded in review (2026-09-25): every row below is delivered in one pull request, and each workstream keeps its own criterion inside it. The order and dependencies are in [the implementation plan](2026-09-25-issue-690-implementation-plan.md).

| PR | Content | Depends on |
|---|---|---|
| A | W1, the post-condition, W7(a), criteria 1 and 2, SPEC-004 §9 draft, CHANGELOG. Closes #690. | none |
| B | W2, criterion 3. | none |
| C | W3, criterion 4, `docs/07` §6. | A |
| D | W4, criterion 5. | A |
| E | W5, criterion 6, `docs/11`. | D |
| F | W6, criterion 7, cache layout version, one-release note, ecosystem CI. | none; independent of A to E |
| G | W7(b), with its own design record. | A |

F is separated from the inheritance work because its risk is different. It changes what dependencies compile against, and it needs the ecosystem measurement before merge. B is separate from A so that its requirement keeps its own criterion.

---

## 8. Mitigation for current releases

Until PR A is released, write the workspace macro as flags on both channels:

```toml
[workspace.build]
cxxflags = ["-DWORKSPACE_DEFINE=1"]
cflags   = ["-DWORKSPACE_DEFINE=1"]   # required when any member has C sources
```

Measured on 2026.9.21.3: `cxxflags` alone reaches the sibling's C++ units but not its C units, and with both lines both positions build. Until PR F is released, a root should not place headers whose names collide with system or dependency headers in its include directories. If a dependency behaves differently after such a header was present, remove that dependency's entry under `~/.mcpp/build-cache/v1/pkg/`.

---

## 9. Open items

1. The ecosystem measurement for W6 (section 5.6) has not been run. It is a merge condition of PR F.
2. The `~/.mcpp` glibc 2.44.3 payload on the measuring machine is corrupted (section 1). It is unrelated to this plan, but it should be repaired before local e2e results from that machine are relied upon.
