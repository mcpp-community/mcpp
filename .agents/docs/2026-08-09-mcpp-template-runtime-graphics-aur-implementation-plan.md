# mcpp Template, Runtime, Graphics, and AUR Convergence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在一张 mcpp Draft PR 中落地已冻结的模板 selector、项目级 RuntimeSelection/RuntimeBinding、provider-neutral 图形运行时契约与仅管理 mcpp-bin 的 AUR reconciler；随后完成 review-gated 普通合入、release、mcpp-index/GitCode 对接和隔离环境下的 xlings 全生态验证。

**Architecture:** CLI 和 mcpp.toml 共用一个 exact dotted PackageSelector；scaffold 在任何落盘前解析出完整 PackageId/version/template，并在 sibling 临时目录事务生成。运行时只允许 McppDefault 或 root/workspace-root NamedSubos，两者解析成一次性 RuntimeBinding snapshot，进入构建指纹并由 build/run/test 复用，dependency/member 的 SubOS 不传递。mcpp-index 声明图形 RuntimeRequirement，xlings/xim 解析 provider，mcpp 仅记录 canonical requester/provider/artifact provenance 和执行平台通用链接/运行验证。GitHub Release manifest 是 AUR desired state，mcpp-bin reconciler 单调、幂等、校验实物并只做 fast-forward push。

**Tech Stack:** C++23 modules, mcpplibs.cmdline, nlohmann/json, TOML manifest parser, Bash/Python 3 release tooling, GitHub Actions, Arch makepkg, gh/git, xlings/xim/mcpp-index.

## Global Constraints

- [ ] 所有行为改动遵循 RED → GREEN → refactor；每个 RED 命令和预期失败原因写入提交/验证台账。
- [ ] mcpp 实现只使用一张 PR，基于最新 `origin/main`；不直接 push main，不 force-push，不 amend/rebase 历史。
- [ ] 所有改动先形成可审阅的独立 commit，立即 push 到 Draft PR，并追加验证/checkpoint 评论；不在提交、PR 或日志摘录中记录本地用户名、绝对工作区路径或私有信息。
- [ ] Draft PR 经用户 review 后才可转 ready；不使用 admin/bypass，只有最新 PR HEAD 的 required checks 全部终态成功后才进行普通 merge。
- [ ] `mcpp-m` 边界为字节级不变：不修改 `scripts/aur/mcpp-m/**`，不读取/生成/发布其内容，不访问其 AUR remote。
- [ ] 不增加 `--variant`、`--subos` 或其他顶层 CLI；不在 mcpp 中出现 GPU vendor、Mesa、NVIDIA、WSL 或 ICD 选择分支。
- [ ] SubOS 只来自本次构建 root/workspace root 的 `mcpp.toml`，不写入 dependency requirement/lock identity，不从 dependency/member 继承。
- [ ] 平台物理规则放入 `src/platform/` 或已有平台模块；Linux ELF/glibc 校验在 macOS/Windows 明确 no-op，不用 Linux 推断替代原生 CI。
- [ ] 所有 stateful 本地/生态验证使用隔离 `HOME`、`MCPP_HOME`、`XLINGS_HOME` 和 SubOS root；验证前后确认宿主状态未变化。
- [ ] 只显式 stage 本计划列出的文件；保留主 checkout 中用户的未跟踪 issue triage 文档。
- [ ] GitHub release 是主发布真源；AUR 暂时不可用时 GitHub release 不回滚，reconciler 留下可重试的精确失败分类。

---

## Task 1: Freeze Baseline, Issue, Branch, and Test Evidence

**Files:**

- Modify: `.agents/docs/2026-08-09-mcpp-template-runtime-graphics-aur-implementation-plan.md`
- Add: `.agents/docs/2026-08-09-mcpp-template-runtime-graphics-aur-validation.md`
- Preserve: `.agents/docs/2026-08-09-mcpp-template-runtime-graphics-aur-focused-design.md`
- Preserve: `.agents/docs/2026-08-09-xlings-mcpp-ecosystem-convergence-design.md`

- [x] Create focused issue #398 referencing #397, #380, #392, and #396.
- [x] Create isolated worktree `feat/template-runtime-graphics-aur` from `origin/main@80291ca`.
- [x] Record baseline versions, latest releases, open PRs, and exact hashes for mcpp, xlings, mcpp-index, and xim-pkgindex in the validation ledger.
- [x] Run baseline `mcpp build`, unit suite, focused scaffold E2E, runtime E2E, AUR script tests (if any), and `git diff --check`; record any pre-existing failure without weakening later gates.
- [x] Snapshot hashes of `scripts/aur/mcpp-m/**` and the host xlings configuration for final boundary comparison.
- [x] Commit the approved design documents, implementation plan, and baseline validation ledger as the first explicit-files commit.

## Task 2: Replace Candidate Guessing with One Exact PackageSelector

**Files:**

- Modify: `src/pm/dependency_selector.cppm`
- Modify: `src/pm/commands.cppm`
- Modify: `src/manifest/toml.cppm`
- Modify: `src/manifest/xpkg.cppm`
- Modify: `src/pm/dep_spec.cppm`
- Modify: `src/pm/index_route.cppm`
- Modify: `src/build/prepare.cppm`
- Modify: `src/cli.cppm`
- Modify: `tests/unit/test_pm_compat.cpp`
- Modify: `tests/unit/test_pm_index_route.cpp`
- Modify: `tests/unit/test_manifest.cpp`
- Modify: `tests/unit/test_pm_package_fetcher.cpp`
- Modify: `tests/e2e/12_add_command.sh`
- Modify: `tests/e2e/27_namespace_dependencies.sh`
- Modify: `tests/e2e/62_dotted_dependency_selector_priority.sh`
- Modify: `tests/e2e/63_bare_dependency_peer_root_priority.sh`
- Modify: `tests/e2e/78_test_main_combinations.sh`
- Modify: `tests/e2e/79_gtest_regular_dep_feature_main.sh`
- Modify: `tests/e2e/162_bare_name_namespace_scope.sh`
- Modify: `tests/e2e/165_bare_name_cross_namespace_wire_address.sh`
- Add: `tests/e2e/203_exact_selector_lock_migration.sh`
- Modify: `docs/05-mcpp-toml.md`
- Modify: `docs/zh/05-mcpp-toml.md`
- Modify: `docs/06-workspace.md`
- Modify: `docs/zh/06-workspace.md`
- Modify: `docs/spec/package-identity.md`

**Interfaces:**

- `PackageSelector { optional<NamespacePath> namespace; NameAtom name; string spelling; }`
- `parse_package_selector(string_view) -> expected<PackageSelector, SelectorError>`
- `normalize_package_selector(PackageSelector, defaultNs="mcpplibs") -> DependencyCoordinate`
- `format_package_selector(DependencyCoordinate) -> dotted selector`

- [x] RED: add unit rows proving `lua -> (mcpplibs,lua)`, `capi.lua -> (capi,lua)`, `mcpplibs.capi.lua -> (mcpplibs.capi,lua)`, and rejection of empty/double-dot/control segments.
- [x] RED: update add E2E so `mcpp add capi.lua@5.4.7` probes/writes exact `capi:lua`, not `mcpplibs.capi:lua`; prove a same-short-name sibling cannot win.
- [x] Implement the O(length), no-I/O shared parser/normalizer and return one exact coordinate after default namespace filling.
- [x] Route `mcpp add`, dependency TOML parsing, feature dependency parsing, and xpkg dependency parsing through the same normalized coordinate.
- [x] Preserve already-parsed/locked identities; during the migration release, emit a warning only when an old dotted candidate exists and differs from the new exact coordinate, with both copyable selectors.
- [x] Update existence-gate and not-found diagnostics to show the normalized PackageId and explicitly mention default `mcpplibs` when namespace was omitted.
- [x] GREEN: run the focused unit and namespace/add E2E tests, then `git diff --check`.
- [x] Commit exact selector normalization and identity documentation.

## Task 3: Make TemplateSpec Typed, Namespace-Aware, and Deterministic

**Files:**

- Modify: `src/scaffold/template.cppm`
- Modify: `src/scaffold/create.cppm`
- Modify: `src/cli/cmd_new.cppm`
- Modify: `src/cli.cppm`
- Add: `tests/unit/test_scaffold.cpp`
- Modify: `tests/e2e/69_package_templates.sh`
- Modify: `.github/workflows/ci-fresh-install.yml`

**Interfaces:**

- `TemplateSpec { PackageSelector package; optional<ExactVersion> version; optional<NameAtom> templateName; bool legacyList; }`
- `parse_template_spec(string_view) -> expected<TemplateSpec, TemplateSpecError>`
- `ResolvedTemplatePackage { DependencyCoordinate id; string version; string indexRoute; string descriptorDigest; string payloadDigest; path root; }`
- `TemplateSelection { ResolvedTemplatePackage package; string templateName; }`

- [x] RED: unit-test every valid and invalid grammar row from focused design §5.1, including multiple `:`/`@`, empty components, exact dotted namespaces, and `pkg:` legacy list recognition.
- [x] RED: add E2E packages with `mcpplibs:widget`, `acme:widget`, and `mcpplibs.capi:lua`; prove template resolution uses the exact PackageId through `IndexRoute::lookup_descriptor`.
- [x] Implement TemplateSpec parsing in the fixed order template delimiter → version delimiter → shared PackageSelector; reject before config/network access.
- [x] Replace scaffold short-name/compat probing with `IndexRoute` exact lookup and keep canonical namespace, version, route, descriptor digest, payload digest, and root through fetch/render/output.
- [x] Reuse package-manager semver resolution for latest stable; omitted versions must not select prereleases, while explicit exact prerelease remains allowed.
- [x] RED: test default selection rules: one explicit default wins; one sole non-default auto-wins; multiple without default list choices and fail; multiple defaults fail validation; no templates directory reports provider error.
- [x] Implement the default rules and `pkg:` one-release warning pointing to `--list-templates`.
- [ ] Update human and machine output to include canonical selector, resolved namespace/name/version, template, and runtime selection.
- [ ] GREEN: run new unit tests, template E2E, and three-platform fresh-install template lanes.
- [x] Commit typed TemplateSpec and exact template-package resolution.

## Task 4: Make Scaffolding Safe and Transactional (#380)

**Files:**

- Add: `src/scaffold/project_name.cppm`
- Add: `src/platform/project_name.cppm`
- Add: `src/platform/scaffold_fs.cppm`
- Modify: `src/scaffold/template.cppm`
- Modify: `src/scaffold/create.cppm`
- Modify: `src/manifest/toml.cppm`
- Modify: `src/pm/commands.cppm`
- Modify: `tests/unit/test_scaffold.cpp`
- Modify: `tests/unit/test_manifest.cpp`
- Modify: `tests/e2e/69_package_templates.sh`
- Add: `tests/e2e/204_new_transactional_scaffold.sh`

**Interfaces:**

- `validate_project_name(string_view) -> expected<PortableProjectName, ProjectNameError>`
- `render_tokens(string_view, RenderVars) -> expected<string, RenderError>`
- `ScaffoldTransaction::begin(parent,name)`, `commit()`, destructor rollback.

- [x] RED: reject empty, absolute, separators, `.`, `..`, C0/DEL, Windows reserved device names, trailing dot/space, quote/tab and names containing the legacy `PROJECT` marker before target creation.
- [x] RED: prove inserted values containing placeholder-like text are not rescanned and all RenderVars render canonical project/template identities.
- [x] RED: inject read/write/copy/rename failures and assert neither final target nor sibling temporary directory remains.
- [x] Implement shared portable project-name validation with platform-specific reserved-name rules isolated under `src/platform/project_name.cppm`.
- [x] Replace repeated string substitution with a single-pass token renderer that never rescans inserted values.
- [x] Extend RenderVars with project namespace/qualified name and template package namespace/name/selector/version/template.
- [x] Replace substring dependency detection with structured manifest editing keyed by canonical PackageId; exact resolved version and features must be idempotent across same short names in different namespaces.
- [x] Generate builtin and package templates in a same-parent temporary directory, check every filesystem/stream operation, fsync/close as supported, and atomically rename only after validation.
- [x] GREEN: run scaffold unit/E2E tests, reproduce #380 cases with timeouts, and confirm no path escape or partial output.
- [ ] Commit transactional scaffold and close #380 from the final PR only after CI.

## Task 5: Introduce Root-Local RuntimeSelection and One RuntimeBinding Snapshot

**Files:**

- Add: `src/xlings/runtime_selection.cppm`
- Add: `src/platform/runtime_binding.cppm`
- Modify: `src/build/prepare.cppm`
- Modify: `src/build/plan.cppm`
- Modify: `src/build/execute.cppm`
- Modify: `src/xlings/subos_info.cppm`
- Modify: `src/toolchain/model.cppm`
- Modify: `src/toolchain/detect.cppm`
- Modify: `src/toolchain/fingerprint.cppm`
- Inspect (no change required; policy is owned by `runtime_selection`): `src/project.cppm`
- Add: `tests/unit/test_runtime_selection.cpp`
- Modify: `tests/unit/test_subos_info.cpp`
- Modify: `tests/unit/test_fingerprint.cpp`
- Add: `tests/e2e/205_root_local_subos.sh`

**Interfaces:**

- `RuntimeSelection { enum Mode { McppDefault, NamedSubos }; string subosName; Source source; path ownerRoot; }`
- `RuntimeBinding { int schema; string providerId; string platform; string arch; string contractHash; optional<path> loader; optional<string> libc; vector<path> libraryDirs; vector<EnvDecl> environment; vector<string> capabilities; string provenance; }`
- `select_runtime(rootManifest, optional workspaceManifest, rootPath) -> expected<RuntimeSelection,string>`
- `resolve_runtime_binding(selection, compiler, GlobalConfig) -> expected<RuntimeBinding,string>`

- [x] RED: no `[xlings].subos` selects `McppDefault` even if active SubOS differs; an explicit `subos = "default"` is a NamedSubos selection.
- [x] RED: missing named SubOS is a hard error and cannot fall back to default/active/compiler-baked runtime.
- [x] RED: workspace root SubOS overrides member declaration during workspace build; a member declaration applies only when that member is built independently.
- [x] RED: dependency manifest SubOS never merges into the consumer, lockfile, or cache identity; the same sources under el8/trixie produce distinct RuntimeBinding contract hashes.
- [x] Implement selection before workspace member substitution and preserve its owner root; materialize project xlings config from the root/workspace-root selection only.
- [x] Define McppDefault as the mcpp-managed default SubOS/runtime in configured xlings home, independent of xlings active/current symlink; bootstrap it when absent.
- [x] Read one RuntimeBinding snapshot, canonicalize/sort its fields, compute contractHash, store it in BuildContext/BuildPlan/cache metadata, and feed it to toolchain detection/fingerprint.
- [x] Reuse that snapshot for run/test environment; remove build/run fast-path re-reads of active SubOS.
- [x] Match current xlings `op=set` presence semantics: preserve any ambient value, including an explicitly empty value, while `prepend` remains ordered and de-duplicated.
- [x] On macOS/Windows return platform-native bindings without invented glibc/ELF fields; platform-specific derivation stays in `src/platform/runtime_binding.cppm`.
- [x] GREEN: run unit tests, named/default/workspace/dependency SubOS E2E, and compare binding/fingerprint output.
- [x] Commit root-local runtime selection and shared binding snapshot.

## Task 6: Fix Runtime Payload Selection and Add Linux Artifact Physics (#392/#396)

**Files:**

- Modify: `src/toolchain/post_install.cppm`
- Modify: `src/toolchain/lifecycle.cppm`
- Add: `src/platform/elf_runtime.cppm`
- Modify: `src/platform/runtime_binding.cppm`
- Add: `src/build/runtime_validation.cppm`
- Modify: `src/build/ninja_backend.cppm`
- Modify: `src/build/execute.cppm`
- Modify: `src/doctor.cppm`
- Modify: `src/xlings/subos_info.cppm`
- Add: `tests/unit/test_elf_runtime.cpp`
- Add: `tests/e2e/206_runtime_binding_physics.sh`

**Interfaces:**

- `ElfRuntimeFacts { artifact; interp; runpaths; needed; requiredGlibcVersions; definedGlibcVersions; resolvedLibc; resolvedObjects; }`
- `validate_runtime_artifact(path, RuntimeBinding, RuntimeResolution) -> RuntimeVerdict`
- `RuntimeVerdict { Pass | ProvenMismatch | Inconclusive; diagnostics[]; }`

- [x] RED: select the glibc payload named by RuntimeBinding, never the first directory entry; stale/absent payload is an explicit error.
- [x] RED: fixture ELFs prove Rule B rejects interpreter/libc from different payloads and accepts same-payload paths.
- [x] RED: fixture version tables prove a required GLIBC symbol floor above the selected libc exports is a hard proven mismatch; a lower/equal floor passes; unavailable closure data is inconclusive, not falsely green.
- [x] Implement semantic exact payload lookup in post-install fixup and bump the fixup revision so existing toolchains repair against the selected binding.
- [x] Implement internal ELF64 little-endian parsing for PT_INTERP, DT_RPATH/RUNPATH, DT_NEEDED and GNU version need/definition sections under `src/platform/elf_runtime.cppm`; no shell parsing on the build hot path.
- [x] Validate only newly linked Linux ELF outputs; cache the verdict by artifact stat/link fingerprint so hot no-op performs zero parses.
- [x] Emit canonical requester/provider/artifact paths and a copyable SubOS remediation; hard-fail proven Rule B/A mismatches, classify unresolvable host/hardware closure as inconclusive with an explicit diagnostic.
- [x] Ensure macOS/Windows validators compile to a typed no-op and never apply Linux glibc rules.
- [x] Extend doctor/runtime explanation to reuse stored verdict rather than re-probe or guess.
- [x] GREEN: run ELF unit fixtures, form-X E2E, current #392 reproduction shape, and a safe host-DSO control.
- [x] Commit runtime physics validation; only close #392/#396 if the final released E2E proves their exact acceptance cases.

## Task 7: Carry Provider-Neutral Graphics Runtime Provenance

**Files:**

- Modify: `src/manifest/types.cppm`
- Modify: `src/manifest/toml.cppm`
- Modify: `src/manifest/xpkg.cppm`
- Modify: `src/build/plan.cppm`
- Modify: `src/build/flags.cppm`
- Modify: `src/build/prepare.cppm`
- Modify: `src/build/runtime_validation.cppm`
- Modify: `src/doctor.cppm`
- Modify: `src/platform/runtime_binding.cppm`
- Modify: `src/xlings/subos_info.cppm`
- Add: `tests/unit/test_runtime_contract.cpp`
- Modify: `tests/unit/test_link_model_runtime_dirs.cpp`
- Modify: `tests/unit/test_runtime_selection.cpp`
- Modify: `tests/unit/test_subos_info.cpp`
- Modify: `tests/e2e/62_runtime_library_dirs.sh`
- Modify: `tests/e2e/66_runtime_provides.sh`
- Modify: `tests/e2e/200_subos_env_reaches_program.sh`
- Modify: `tests/e2e/205_root_local_subos.sh`
- Add: `tests/e2e/207_runtime_contract_provenance.sh`
- Modify: `docs/05-mcpp-toml.md`
- Modify: `docs/zh/05-mcpp-toml.md`
- Modify: `docs/08-toolchain-internals.md`
- Modify: `docs/zh/08-toolchain-internals.md`

**Interfaces:**

- `RuntimeRequirement { kind; value; phase; canonical requester PackageId; required; }`
- `RuntimeArtifact { role; canonical provider PackageId; path; provenance; abi; digest; hostFingerprint; }`
- `LinkIntent { libraries; linkLibraryDirs; transitiveNeededDirs; runtimeSearchDirs; frameworks; deployFiles; }`

- [x] RED: same-short-name providers in two namespaces remain distinguishable in runtime resolution JSON and `mcpp why runtime`.
- [x] RED: required capabilities and provided capabilities are separate; a requester cannot become its own provider merely because it requires a capability.
- [x] RED: runtime search dirs do not enter `-L`; Linux transitive-needed dirs use `-Wl,-rpath-link`, macOS emits rpath/install-name semantics, and Windows uses explicit deploy files.
- [x] Introduce structured generic requirement/artifact/provenance values while keeping legacy descriptor fields readable for one compatibility train.
- [x] Populate requester/provider from the resolved package identity, including namespace/version/index provenance; never use bare `package.name` as provider identity.
- [x] Write the resolved RuntimeBinding, requirements, artifacts, search mechanism, and validation verdict into `resolution.json`.
- [x] Make `mcpp why runtime` a pure interpreter of stored generic facts; GPU/driver diagnostics point to xlings and never probe hardware.
- [x] Add a static ownership gate that rejects new mcpp source branches containing provider-specific GPU/ICD selection vocabulary outside docs/tests.
- [x] GREEN: run runtime-contract unit/E2E tests and prove the build hot path launches no GL/Vulkan probe.
- [x] Commit provider-neutral runtime contract and link-intent separation.

## Task 8: Generate an Immutable Release Manifest

**Files:**

- Add: `scripts/release/generate_manifest.py`
- Add: `tests/scripts/test_release_manifest.py`
- Modify: `.github/workflows/release.yml`
- Modify: `docs/09-release.md`
- Modify: `docs/zh/09-release.md`

**Interface:**

- Release asset `mcpp-release.json` schema 1 with version, tag, release commit, and exact name/SHA256 for Linux x86_64/aarch64 plus every shipped platform asset.

- [x] RED: fixtures reject duplicate platform/arch rows, missing sidecars, mismatched hashes, draft/prerelease input, wrong tag/version, and non-deterministic ordering.
- [x] Implement deterministic manifest generation from downloaded release artifacts and sidecars, recomputing every SHA256.
- [x] Wire the release workflow so the manifest is uploaded only after all required release assets exist and validation passes.
- [x] Add a release gate that downloads the uploaded manifest and compares it to the final GitHub release inventory.
- [x] GREEN: run manifest tests and a local fixture generation twice with byte-identical output.
- [x] Commit immutable release desired-state manifest.

## Task 9: Replace Combined AUR Publishing with an mcpp-bin Reconciler

**Files:**

- Add: `scripts/aur/reconcile_mcpp_bin.py`
- Add: `scripts/aur/render_mcpp_bin.py`
- Add: `tests/scripts/test_aur_reconcile.py`
- Modify: `scripts/aur/update.sh`
- Modify: `scripts/aur/README.md`
- Modify: `.github/workflows/aur-publish.yml`
- Preserve byte-for-byte: `scripts/aur/mcpp-m/**`

**Interface:**

- `inspect -> DesiredState/ObservedState/ReconcilePlan`
- exit classification `noop | updated | transient | permanent | refused-downgrade`
- manual inputs `publish=false|true`, optional exact latest stable tag only; no downgrade override in this phase.

- [x] RED: fixture tests cover desired==observed no-op, upgrade, late old event refusal, missing/hash-mismatched asset, AUR maintenance retry, auth/permanent failure, RPC lag after git update, and known clone failure not becoming first publish.
- [x] RED: assert the reconciler never opens, hashes, copies, stages, or addresses `scripts/aur/mcpp-m/**`; compare pre/post tree hashes.
- [x] Split `update.sh` into an mcpp-bin-only compatibility wrapper around the renderer; remove all mcpp-m reads/writes without editing mcpp-m files.
- [x] Render PKGBUILD from `mcpp-release.json`; regenerate `.SRCINFO` with non-root Arch `makepkg --printsrcinfo`, then `makepkg --verifysource`.
- [x] Query AUR RPC and HTTPS git, compare versions with Arch `vercmp`, validate both Linux assets/sidecars, and produce a dry-run diff before secrets are loaded.
- [x] Publish only by normal fast-forward SSH push with pinned AUR host key and bounded exponential retry for maintenance/timeouts; never force or initialize a missing known package.
- [x] Verify remote git head, bounded-poll RPC, then install in a clean Arch container and assert `mcpp --version`.
- [x] Change workflow triggers to successful release workflow_run + six-hour schedule + workflow_dispatch; all call the same latest-stable reconciler. Remove the mcpp-m publish leg.
- [x] Emit Actions summary with trigger, desired/observed versions, hashes, remote commit, retry count, classification, and drift age; AUR failure must not alter GitHub release conclusion.
- [ ] GREEN: run Python tests, shell lint, dry-run against current latest release, and an Arch container source verification.
- [x] Commit mcpp-bin-only AUR reconciliation and verify mcpp-m byte hashes are unchanged.

## Task 10: Pin Latest xlings, Version mcpp, and Update User Documentation

**Files:**

- Modify: `src/xlings.cppm`
- Modify: `.github/actions/bootstrap-mcpp/action.yml`
- Modify: `.github/workflows/bootstrap-macos.yml`
- Modify: `.github/workflows/ci-linux-e2e.yml`
- Modify: all other authoritative xlings pin sites found by `tests/unit/test_xlings_version_pin.cpp`
- Modify: `mcpp.toml`
- Modify: `README.md`
- Modify: `docs/00-getting-started.md`
- Modify: `docs/zh/00-getting-started.md`
- Modify: `docs/05-mcpp-toml.md`
- Modify: `docs/zh/05-mcpp-toml.md`
- Modify: `docs/08-toolchain-internals.md`
- Modify: `docs/zh/08-toolchain-internals.md`
- Modify: `docs/spec/package-identity.md`
- Modify: `scripts/aur/README.md`

- [x] Re-query latest non-draft/non-prerelease xlings immediately before pinning; pin the exact version and verify every authoritative pin site matches.
- [x] Choose the next unused calendar version (expected `2026.8.9.1` after live tag check), update `mcpp.toml`, and leave AUR snapshots to release-time generation.
- [x] Document `[ns.]name[@version][:tname]`, default `mcpplibs`, exact dotted namespaces, sole-template default, and legacy list migration.
- [x] Document McppDefault vs root/workspace-root `[xlings].subos`, no CLI override, non-transitive dependency semantics, coexistence across glibc bindings, and prebuilt ABI metadata boundary.
- [x] Document xlings/xim ownership of OpenGL/Vulkan providers and that mcpp never probes GPU/driver/ICD.
- [x] Document mcpp-bin-only eventual AUR reconciliation and explicitly state mcpp-m/mcpp-git are outside automation.
- [x] Update English and Chinese examples together; regenerate command reference if CLI help changed.
- [ ] GREEN: run pin tests, docs example tests, generated command reference tests, `git diff --check`, and forbidden-vocabulary/boundary scans.
- [ ] Commit version, xlings pin, and documentation.

## Task 11: Full Local Validation and PR Publication

**Files:**

- Modify: `.agents/docs/2026-08-09-mcpp-template-runtime-graphics-aur-validation.md`

- [ ] Build a fresh mcpp binary with the pinned xlings in isolated homes; do not validate with a stale installed mcpp.
- [ ] Run the complete unit suite and every applicable Linux E2E, then focused no-cache workspace/self-host/release builds.
- [ ] Run scaffold/template, default/named/workspace SubOS, runtime physics, runtime provenance, release-manifest, and AUR reconcile gates independently and record commands/results.
- [ ] Run sanitizers/static checks available in repository CI, `git diff --check origin/main...HEAD`, forbidden mcpp-m diff, and explicit scope inventory.
- [ ] Measure hot no-op build and template parse against baseline; require no material regression and zero xlings/GPU subprocess on hot no-op.
- [ ] Rebase is forbidden; if origin/main advanced, merge origin/main normally, rerun all affected gates, and preserve visible history.
- [x] Push the branch and open one Draft PR with `Closes #398`, related issue notes, architecture decisions, RED/GREEN evidence, boundary proof, version/pin, and cross-repo follow-up plan.
- [ ] After every logical commit, push immediately and append a checkpoint with commit id, focused evidence, remaining failures, and cross-repository boundary.
- [ ] Request review using the repository review workflow and address feedback with fresh evidence.

## Task 12: Native GitHub Actions, Review-Gated Merge, and Release

**Skills required at this task:** `mcpp-release`, `superpowers:verification-before-completion`, and `superpowers:finishing-a-development-branch`.

- [ ] Wait for all latest-HEAD checks to reach terminal success across Linux, macOS, Windows, cross/QEMU aarch64, native aarch64, fresh install, release manifest, and AUR dry-run lanes.
- [ ] Treat skipped native/runtime hardware rows as NOT_EXERCISED, not pass; create an isolated temporary CI lane if a required platform gate is absent.
- [ ] Verify remote PR HEAD equals local HEAD, branch diff is clean, no unresolved reviews, no conflict, and `scripts/aur/mcpp-m/**` is unchanged.
- [ ] After explicit user review, mark the Draft ready and use the repository's normal merge path without admin/bypass; verify the merge commit is on main and PR/issue states are correct.
- [ ] Follow `mcpp-release`: verify version strings, create/push the release tag, monitor every release job, asset, checksum, `mcpp-release.json`, GitCode mirror, and GitHub release until terminal success.
- [ ] Do not block/rollback the GitHub release for an AUR transient; dispatch the fixed latest-stable mcpp-bin reconciler and record its exact terminal state.
- [ ] Close #380/#392/#396 only when their released acceptance paths are proven; otherwise comment with delivered subset and keep the residual open.

## Task 13: mcpp-index, GitCode Resources, and xlings Ecosystem Follow-Through

**Repositories:**

- `mcpplibs/mcpp-index`
- `openxlings/xim-pkgindex`
- `openxlings/xlings`
- GitCode helper/config already used by the release workflow

- [ ] In isolated repo-specific worktrees, update mcpp-index minimum/current mcpp pins and template descriptors to canonical selectors and sole/default template lint.
- [ ] Refactor ImGui features/templates to core/headless, backend-glfw-opengl3, backend-vulkan, app, docking, and viewports without making core pull graphics unconditionally.
- [ ] Express OpenGL/Vulkan runtime requirements through generic xlings/xim package/capability dependencies and canonical provider identities; remove mcpp-index host ICD/DSO collection paths.
- [ ] In xim-pkgindex, add/fix Vulkan loader/ICD and host-link provider recipes/provenance only where current recipes cannot satisfy the new generic contract; keep provider detection/lifecycle outside mcpp.
- [ ] Use the established index CI workflow, one repo PR per external repository as required by repository boundaries, and wait for every latest-head validation job before merge.
- [ ] Confirm the mcpp release job opens/merges the mcpp index version bump and local GitCode resources contain the exact new release assets plus SHA256 sidecars; verify GitCode with ranged GET and full SHA256.
- [ ] Publish/rebuild index artifacts and verify their public hashes/refs.
- [ ] From fresh isolated HOME/MCPP_HOME/XLINGS_HOME, install the released mcpp and pinned/released xlings, run `xlings update`, canonical template new/build/run/test, default runtime, two named SubOS/glibc builds, and package lifecycle smoke.
- [ ] Run software OpenGL and Vulkan instance/device lanes where hardware exists; report AMD/Intel/NVIDIA/WSL/macOS/Windows hardware rows as PASS/FAIL/NOT_EXERCISED with actual provider/ICD provenance.
- [ ] Verify mcpp does not probe GPU and the same graphics dependencies resolve through xlings/xim under each exercised environment.
- [ ] Record all PRs, commits, CI run/job IDs, release tags, asset hashes, GitCode URLs, AUR state, and public smoke results in the validation ledger.

## Task 14: Completion Audit and Final Report

- [ ] Verify mcpp main/release/index heads, tags, PR merge states, issue comments/states, AUR mcpp-bin desired/observed state, and public artifact hashes from live sources.
- [ ] Verify local worktrees are clean and main checkout user files remain untouched.
- [ ] Re-run `git diff --check`, version/pin consistency, mcpp-m boundary hash, and one final cold-home released-binary smoke.
- [ ] Mark every checkbox with evidence; do not convert NOT_EXERCISED hardware rows into green.
- [ ] Update the active goal to complete only when no required item remains.
- [ ] Report concise core outcomes plus links to the implementation plan, validation ledger, issue, PR, CI, release, index PRs/artifacts, GitCode, AUR and ecosystem evidence.
