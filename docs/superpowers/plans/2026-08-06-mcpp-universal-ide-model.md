# mcpp 通用 IDE 工程模型实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 mcpp 在普通源码编译前发布带有真实工程模型和正确编译参数的 CDB，并在后台准备模块/生成产物，让 VS Code 和其他客户端不再从 CDB 或人类日志反推工程语义。

**Architecture:** 保留 M0 `inspect` 作为只读声明入口，新增显式 `configure` 和 `prepare`。`configure` 复用 mcpp 的解析链路，在 BuildPlan 和 CompileFlags 形成后先发布 fresh-only CDB；`prepare` 使用最小 Ninja goals 生成 generated outputs、std/工程 BMI，再发布 ready snapshot。协议采用 mcpp 自有版本化 JSON/NDJSON，公共模型预留到 BSP 的映射，但第一版不实现 daemon 或 BSP server。

**Tech Stack:** C++23 named modules、现有 `std::expected`/`std::filesystem`、nlohmann JSON module、Ninja backend、GoogleTest、portable Bash E2E、VS Code Extension Host、TypeScript、官方 clangd。

---

## 文件地图

| 文件 | 责任 |
| --- | --- |
| `src/ide/model.cppm` | schema 1 的 project/configuration/target/dependency/toolchain/artifact/diagnostic DTO |
| `src/ide/snapshot.cppm` | inspection/snapshot JSON、ID 和兼容字段 |
| `src/ide/events.cppm` | NDJSON event envelope、sequence、phase/status 和错误事件 |
| `src/ide/publish.cppm` | reply/index/current/last-known-good 原子发布 |
| `src/ide/inspect.cppm` | M0 只读声明和已发布状态读取 |
| `src/ide/configure.cppm` | resolution + BuildPlan + fresh CDB + configured snapshot |
| `src/ide/prepare.cppm` | configure 复用、artifact goals、ready snapshot |
| `src/build/resolution.cppm` | 从 `prepare_build()` 提取 resolved packages、edges、features 和 provenance |
| `src/build/artifact_layout.cppm` | BMI/std/generated/action 路径及 IDE goal 计算 |
| `src/build/compile_commands.cppm` | fresh-only CDB 发射；保留 legacy merge API |
| `src/build/backend.cppm` | emit/build 分离及跳过 link preflight 的 options |
| `src/build/ninja_backend.cppm` | build.ninja/CDB emission、显式 goal 执行和结果记录 |
| `src/build/prepare.cppm` | 解析流程拆分为 resolve、plan、materialize、execute 可复用阶段 |
| `src/toolchain/stdmod.cppm` | 纯 std artifact 描述与实际 ensure/build 分离 |
| `src/cli/cmd_ide.cppm` | inspect/configure/prepare CLI 路由和 selectors |
| `src/cli.cppm` | help、命令注册和 snapshot compatibility alias |
| `tests/unit/test_ide_protocol.cpp` | DTO、ID、event、publisher、schema fixture |
| `tests/unit/test_ide_configure.cpp` | CDB 提前发布和 no-ordinary-build 契约 |
| `tests/unit/test_ide_prepare.cpp` | minimal goals、artifact state 和回退 |
| `tests/e2e/198_ide_configure.sh` | fresh CDB、错误源码、事件顺序和零链接证明 |
| `tests/e2e/199_ide_prepare.sh` | generated/BMI、last-known-good、取消和多配置 |
| `/Users/cltx/projects/mcpp/mcpp-vscode/src/ideProtocol.ts` | 插件协议进程、NDJSON parser、snapshot client |
| `/Users/cltx/projects/mcpp/mcpp-vscode/src/ideWorkflow.ts` | configure/prepare 生命周期和状态机 |
| `/Users/cltx/projects/mcpp/mcpp-vscode/src/extension.ts` | 打开工程、watcher、clangd 配置和 UI 生命周期 |
| `/Users/cltx/projects/mcpp/mcpp-vscode/src/cliController.ts` | 保留 build/test/run，增加 IDE operation execution |
| `/Users/cltx/projects/mcpp/mcpp-vscode/test/ideProtocol.test.ts` | parser、version/capability/fallback tests |
| `/Users/cltx/projects/mcpp/mcpp-vscode/test/ideWorkflow.test.ts` | configured/ready/stale/cancel/late-event tests |
| `/Users/cltx/projects/mcpp/mcpp-vscode/test/extension-host/` | 真实 VS Code + fake/real mcpp integration fixtures |

## 任务 1：锁定 schema 1 和 operation model

**Files:**
- Modify: `src/ide/model.cppm`
- Modify: `src/ide/snapshot.cppm`
- Create: `src/ide/events.cppm`
- Create: `tests/unit/test_ide_protocol.cpp`
- Create: `tests/fixtures/ide/project-v1.json`
- Create: `tests/fixtures/ide/configured-v1.json`
- Create: `tests/fixtures/ide/events-v1.ndjson`

- [ ] **Step 1: 写失败测试固定 ID、phase、state 和 event wire names**

测试必须锁定：`declared`、`configured`、`ready`、`stale`、`unavailable`，以及 `operation-started`、`progress`、`diagnostic`、`snapshot-published`、`operation-finished`。测试输入使用固定绝对路径 `/workspace/app` 和固定 selector，断言同一输入生成相同 `projectId`/`configurationId`，改变 toolchain fingerprint 后只改变 `snapshotId`。

- [ ] **Step 2: 运行 focused test 确认 RED**

运行：

```bash
mcpp test ide_protocol
```

预期：失败，因为 phase/state/event DTO 尚未导出。

- [ ] **Step 3: 实现最小公共 DTO 和 canonical serialization**

新增导出类型：`ProjectId`、`ConfigurationId`、`SnapshotId`、`IdePhase`、`ArtifactStatus`、`IdeEventType`、`OperationStatus`、`ConfigurationSelectors`、`ToolchainIdentity`、`TargetModel`、`DependencyModel`、`ArtifactModel`、`IdeSnapshot`、`IdeEvent`。ID 计算函数必须接收 canonical JSON/text，而不是内部对象地址或 wall-clock time。

- [ ] **Step 4: 写 fixture parser 和 additive-field test**

用 nlohmann JSON 读取 fixture，断言未知字段不影响解析，缺少必需 `schemaVersion/kind/projectId/configurationId` 时返回稳定 `MCPP_IDE_SCHEMA_INVALID`。NDJSON parser 必须逐行处理，空行跳过，非对象行和重复/倒退 `seq` 返回错误。

- [ ] **Step 5: 运行 focused test 确认 GREEN 并提交**

运行 `mcpp test ide_protocol`，预期所有 schema/ID/event 测试通过。

提交：

```bash
git add src/ide/model.cppm src/ide/snapshot.cppm src/ide/events.cppm tests/unit/test_ide_protocol.cpp tests/fixtures/ide
git commit -m "feat(ide): define versioned project and operation protocol"
```

## 任务 2：提取 resolved project model

**Files:**
- Create: `src/build/resolution.cppm`
- Modify: `src/build/prepare.cppm`
- Modify: `src/build/plan.cppm`
- Modify: `src/modgraph/graph.cppm`
- Create: `tests/unit/test_build_resolution.cpp`

- [ ] **Step 1: 为 resolved package/edge/feature/provenance 写失败测试**

构造两个临时 path packages 和一个 feature-gated dependency，调用新的 resolution API，断言返回 member/package IDs、dependency edge visibility、requested/default features、effective target/profile 和 source provenance。测试不得调用 Ninja，也不得创建最终 binary。

- [ ] **Step 2: 运行 `mcpp test build_resolution` 确认 RED**

预期：模块不存在或 API 未导出。

- [ ] **Step 3: 从 `prepare_build()` 提取可复用 resolution 结果**

把当前局部 `DependencyEdge`、resolved `packages`、activated features、toolchain selection、generated/action outputs 和 workspace selection 放进公共结果对象。保持 `BuildContext` 对 build/test 的字段兼容；新增字段只记录来源，不改变现有 build 语义。

- [ ] **Step 4: 将 BuildPlan 投影输入改为 resolved result**

让 `make_plan()` 继续接收内部构建参数，但由 resolution 结果统一提供 packages/edges/topological order。不要让 `mcpp.ide` 直接 import `mcpp.build.plan` 的内部 DTO。

- [ ] **Step 5: 运行 focused tests 和现有 build-plan tests**

运行：

```bash
mcpp test build_resolution
mcpp test plan
```

预期：新测试通过，现有 plan 测试无行为变化。

- [ ] **Step 6: 提交**

```bash
git add src/build/resolution.cppm src/build/prepare.cppm src/build/plan.cppm src/modgraph/graph.cppm tests/unit/test_build_resolution.cpp
git commit -m "refactor(build): expose resolved project model for IDE clients"
```

## 任务 3：分离 artifact layout 和 std BMI 描述

**Files:**
- Create: `src/build/artifact_layout.cppm`
- Modify: `src/build/ninja_backend.cppm`
- Modify: `src/toolchain/stdmod.cppm`
- Modify: `src/build/plan.cppm`
- Create: `tests/unit/test_ide_artifact_layout.cpp`

- [ ] **Step 1: 写失败测试锁定路径和 goal 集合**

给定含一个普通 `.cpp`、一个 module interface、一个 `import std` 的 BuildPlan，断言：

```text
configured CDB entries = all supported source units
IDE goals = generated outputs + std BMI + module BMI/interface object
IDE goals do not contain ordinary implementation object or LinkUnit output
```

测试还要断言两个不同 configuration/fingerprint 不共享 BMI 路径。

- [ ] **Step 2: 运行 focused test 确认 RED**

运行 `mcpp test ide_artifact_layout`，预期 artifact layout API 不存在。

- [ ] **Step 3: 提取 Ninja 私有 BMI/output path 函数**

将 `bmi_path()`、staged std/std.compat 路径、source action outputs 和 object-to-source mapping 收敛到 `mcpp.build.artifact_layout`。Ninja 只消费该模块，不再私自重新计算路径。

- [ ] **Step 4: 把 `stdmod::ensure_built` 拆成 describe/materialize**

新增纯函数返回 std identity、cacheDir、bmi/object/compat 路径和 metadata 输入；`ensure_built()` 调用该描述并负责实际编译。configure 只调用 describe，prepare 才调用 materialize/ensure。

- [ ] **Step 5: 运行 focused test 和 std BMI regression tests**

运行 `mcpp test ide_artifact_layout` 以及现有 `stdmod`/BMI cache tests，确认路径和缓存命中语义保持一致。

- [ ] **Step 6: 提交**

```bash
git add src/build/artifact_layout.cppm src/build/ninja_backend.cppm src/toolchain/stdmod.cppm src/build/plan.cppm tests/unit/test_ide_artifact_layout.cpp
git commit -m "refactor(build): centralize IDE artifact layout and goals"
```

## 任务 4：让 CDB 支持 fresh-only 和提前发布

**Files:**
- Modify: `src/build/compile_commands.cppm`
- Modify: `src/build/ninja_backend.cppm`
- Modify: `src/build/backend.cppm`
- Modify: `src/build/execute.cppm`
- Create: `tests/unit/test_compile_commands_fresh.cpp`

- [ ] **Step 1: 写失败测试证明 fresh CDB 不保留历史条目**

用一个旧 CDB 包含 `/old/test.cpp`，fresh plan 只包含 `/new/main.cpp`，调用 fresh emitter，断言输出只有 `/new/main.cpp`。单独调用旧 `merge_compile_commands()` 的测试保持原行为，证明 legacy build/test compatibility 未被删除。

- [ ] **Step 2: 运行 focused test 确认 RED**

运行 `mcpp test compile_commands_fresh`，预期 fresh API 不存在。

- [ ] **Step 3: 增加 `emit_build_files` 和 `execute_goals` backend 边界**

`NinjaBackend` 提供两个阶段：

```cpp
std::expected<EmittedBuildFiles, BuildError>
emit_build_files(const BuildPlan&, const EmitOptions&);

std::expected<BuildResult, BuildError>
execute_goals(const BuildPlan&, const BuildOptions&);
```

`emit_build_files` 写 build.ninja、fresh CDB 和 generated action metadata；`execute_goals` 才执行 hermetic link preflight（当 goal 集合包含 link output 时）和 Ninja。`dryRun` 不再承担 IDE API 的全部语义。

- [ ] **Step 4: 让普通 `mcpp build` 复用同一 emission**

保持默认 build 的 root CDB compatibility projection 和历史 merge，但实际 fresh content 必须先由同一个 emitter 生成，避免 IDE CDB 与 build CDB 参数漂移。

- [ ] **Step 5: 运行 CDB、backend 和现有 build tests**

运行 `mcpp test compile_commands_fresh`、`mcpp test backend` 和现有 CDB e2e。预期普通 build 行为不变，fresh emitter 无历史污染。

- [ ] **Step 6: 提交**

```bash
git add src/build/compile_commands.cppm src/build/ninja_backend.cppm src/build/backend.cppm src/build/execute.cppm tests/unit/test_compile_commands_fresh.cpp
git commit -m "feat(build): publish fresh compile databases before execution"
```

## 任务 5：实现 snapshot publisher 和 configured/ready index

**Files:**
- Create: `src/ide/publish.cppm`
- Modify: `src/ide/snapshot.cppm`
- Modify: `src/ide/model.cppm`
- Create: `tests/unit/test_ide_publish.cpp`
- Create: `tests/fixtures/ide/publisher-layout.txt`

- [ ] **Step 1: 写失败测试覆盖原子发布和回退**

测试在临时 work root 中发布 configured snapshot，再发布 ready snapshot，断言 index 只引用完整 JSON；模拟失败/取消时 index、current 和 last-known-good 不变，临时文件不会被客户端看到。

- [ ] **Step 2: 运行 `mcpp test ide_publish` 确认 RED**

- [ ] **Step 3: 实现 publisher API**

导出以下 API：

```cpp
std::expected<PublishedSnapshot, PublishError>
publish_snapshot(const SnapshotDocument&, const PublishOptions&);

std::expected<void, PublishError>
publish_last_known_good(const ConfigurationId&, const SnapshotId&);

std::expected<std::optional<SnapshotDocument>, PublishError>
read_current_snapshot(const ConfigurationId&, const std::filesystem::path&);
```

publisher 负责 temp file、flush/close、JSON parse validation、rename、index 引用和摘要校验；不负责解析 manifest 或执行工具。

- [ ] **Step 4: 实现 configured/ready 与 stale 计算**

configured CDB 发布后可成为 `current`，只有 ready 才更新 `last-known-good`。输入摘要变化而旧 snapshot 仍存在时返回 `stale`，不要删除旧回复文件。

- [ ] **Step 5: 运行 publisher tests 并提交**

```bash
mcpp test ide_publish
git add src/ide/publish.cppm src/ide/snapshot.cppm src/ide/model.cppm tests/unit/test_ide_publish.cpp tests/fixtures/ide/publisher-layout.txt
git commit -m "feat(ide): atomically publish configured and ready snapshots"
```

## 任务 6：实现 `mcpp ide configure`

**Files:**
- Create: `src/ide/configure.cppm`
- Modify: `src/cli/cmd_ide.cppm`
- Modify: `src/cli.cppm`
- Modify: `src/ide/model.cppm`
- Create: `tests/unit/test_ide_configure.cpp`
- Create: `tests/e2e/198_ide_configure.sh`

- [ ] **Step 1: 写失败测试锁定事件顺序**

使用临时工程和 fake toolchain，断言 stdout 事件顺序为 `operation-started -> progress(resolve) -> snapshot-published(configured) -> operation-finished`。在 `snapshot-published` 事件之后检查 CDB 已存在；测试记录 fake compiler 尚未收到普通项目 TU 编译调用。

- [ ] **Step 2: 运行 focused test 确认 RED**

运行 `mcpp test ide_configure`，预期 configure module/CLI 不存在。

- [ ] **Step 3: 实现 configure pipeline**

configure 按以下顺序调用：

```text
selectors -> inspect/resolve workspace
         -> resolve_project
         -> describe toolchain/std artifacts
         -> build.mcpp/generated resolution
         -> module graph + BuildPlan
         -> compute_flags
         -> emit fresh CDB
         -> validate entries and paths
         -> publish configured snapshot
```

任何普通项目 compile goal 或 link output 都不得在该命令中执行。不可避免的 host helper/build.mcpp 执行必须以 progress/diagnostic event 标记。

- [ ] **Step 4: 实现 CLI NDJSON 输出和退出码**

stdout 只写合法 NDJSON；人类日志进入 stderr/output channel。成功 configured 返回 0；解析失败、权限失败或无 CDB 且无 last-known-good 返回 3；取消返回 130；CLI 参数错误返回 2。

- [ ] **Step 5: 写 E2E：错误源码仍产生 CDB**

fixture 包含语法错误的 `src/main.cpp`。运行 configure，解析 configured event，检查 CDB entry 的 compiler/directory/arguments；断言最终 binary 和普通 `.o` 不存在。使用一个不会触发网络安装的显式 system toolchain fixture，避免测试依赖外部 index。

- [ ] **Step 6: 补齐失败回退和 cached BMI 工具链矩阵**

失败回退测试先写入带 marker 的旧根 CDB，再通过真实 `configure_project()` 制造 cached BMI staging 失败，断言命令返回结构化诊断、没有发布新 snapshot，并且旧根 CDB 字节不变。任务 5 的 publish 层完成后，再把同一故障注入扩展为 current/last-known-good 均不变的断言。

cached BMI 测试分别在可用的 Clang、GCC、MSVC capability 下预热一个含依赖模块的全局缓存，删除工程 target 后运行 `ide configure`，断言 CDB 引用的 `.pcm`、`.gcm` 或 `.ifc` 已存在，且目标路径与 Ninja 消费的 `artifact_layout` 完全相同。该矩阵验证 IDE staging 和核心后端共享契约，不宣称 clangd 能读取 GCC `.gcm` 或 MSVC `.ifc`。

- [ ] **Step 7: 运行 focused unit/e2e 并提交**

```bash
mcpp test ide_configure
bash tests/e2e/198_ide_configure.sh
git add src/ide/configure.cppm src/cli/cmd_ide.cppm src/cli.cppm src/ide/model.cppm tests/unit/test_ide_configure.cpp tests/e2e/198_ide_configure.sh
git commit -m "feat(ide): add configure command with early CDB publication"
```

## 任务 7：实现 `mcpp ide prepare` 的最小 goals

**Files:**
- Create: `src/ide/prepare.cppm`
- Modify: `src/build/artifact_layout.cppm`
- Modify: `src/build/backend.cppm`
- Modify: `src/build/ninja_backend.cppm`
- Create: `tests/unit/test_ide_prepare.cpp`
- Create: `tests/e2e/199_ide_prepare.sh`

- [ ] **Step 1: 写失败测试固定 goal 过滤**

给定普通 `.cpp`、module interface、generated action 和 binary target，断言 prepare goals 只包含 generated output、std/BMI 和 module interface object/BMI；binary output 和普通 implementation object 不在 goals 中。

- [ ] **Step 2: 运行 focused test 确认 RED**

运行 `mcpp test ide_prepare`，预期 prepare goal selector 不存在。

- [ ] **Step 3: 实现 `ide_artifact_goals(plan)`**

从 `artifact_layout` 返回去重、稳定排序的相对 Ninja outputs。goal 计算必须依据 `CompileUnit::providesModule`、std artifact descriptor 和 `BuildPlan::actions`，不得通过字符串匹配 `build.ninja`。

- [ ] **Step 4: 跳过不必要的 link preflight**

当 `BuildOptions.ninjaTargets` 不包含 `LinkUnit::output` 时，Ninja backend 不运行 hermetic link preflight，也不把 final link 产物记录为 produced artifact。

- [ ] **Step 5: 发布 ready 或保留 configured**

prepare 成功后验证每个 artifact 的 existence、regular-file/type、toolchain fingerprint 和 expected producer；不满足时发布结构化 diagnostic，configured snapshot 仍可使用，last-known-good 不更新。

- [ ] **Step 6: E2E 验证 generated/BMI/无最终链接**

fixture 需要一个 generated header 或 source、一个 module interface 和一个 binary target。运行 prepare，断言 generated file、std/module BMI 存在，普通 implementation `.o` 和 final binary 不存在；允许 host helper、host tool 和 module interface object 存在。

- [ ] **Step 7: 运行测试并提交**

```bash
mcpp test ide_prepare
bash tests/e2e/199_ide_prepare.sh
git add src/ide/prepare.cppm src/build/artifact_layout.cppm src/build/backend.cppm src/build/ninja_backend.cppm tests/unit/test_ide_prepare.cpp tests/e2e/199_ide_prepare.sh
git commit -m "feat(ide): prepare generated files and module artifacts without linking"
```

## 任务 8：移除插件侧工程反推，接入协议客户端

**Files:**
- Create: `/Users/cltx/projects/mcpp/mcpp-vscode/src/ideProtocol.ts`
- Create: `/Users/cltx/projects/mcpp/mcpp-vscode/src/ideWorkflow.ts`
- Modify: `/Users/cltx/projects/mcpp/mcpp-vscode/src/extension.ts`
- Modify: `/Users/cltx/projects/mcpp/mcpp-vscode/src/workflow.ts`
- Modify: `/Users/cltx/projects/mcpp/mcpp-vscode/src/cliController.ts`
- Modify: `/Users/cltx/projects/mcpp/mcpp-vscode/src/analysis.ts`
- Modify: `/Users/cltx/projects/mcpp/mcpp-vscode/package.json`
- Create: `/Users/cltx/projects/mcpp/mcpp-vscode/test/ideProtocol.test.ts`
- Create: `/Users/cltx/projects/mcpp/mcpp-vscode/test/ideWorkflow.test.ts`

- [ ] **Step 1: 写 fake NDJSON client 测试**

输入 configured、ready、stale、diagnostic、cancelled 和 legacy events，断言 parser 拒绝错误 schema/倒退 seq，workflow 正确更新 per-project/per-configuration state，并丢弃旧 operation 的迟到事件。

- [ ] **Step 2: 实现 argv-array protocol client**

使用 `child_process.spawn` 的 argv 数组，cwd 为 selected project root。stdout 逐行解析，stderr 进入现有 output channel；提供 `AbortSignal`，结束时返回 exit code 和 last-known-good ID。不要用正则解析任何 mcpp 人类输出。

- [ ] **Step 3: 接入首次打开和 manifest watcher**

受信任 workspace：`inspect -> configure -> apply configured CDB -> continue prepare -> apply ready`。未受信任 workspace：只读 index/CDB，不执行 mcpp、compiler 或 clangd。manifest/selector/profile/target 变化时按 `projectId + configurationId` 取消旧 operation。

- [ ] **Step 4: 用 snapshot toolchain 配置 clangd**

`configureClangd` 接收 snapshot 的 compiler path、clangd path/identity、compileCommands directory、module capability 和 workspace scope。原有 `analyzeCompilationDatabase()` 仅保留为 CDB syntax/sanity check 和 legacy fallback，不再选择工程 capability。

- [ ] **Step 5: 替换 toolchain list 正则路径**

新增 mcpp protocol capability 可用时，工具链 QuickPick 读取 snapshot 的 `toolchain`/`configurations`；旧 mcpp 才调用 `toolchain list` 和现有 parser。测试覆盖版本协商和旧版 fallback。

- [ ] **Step 6: 更新命令和设置**

新增 `mcpp.configureIde`、`mcpp.prepareIde` 命令；把“刷新编译数据库”改为 configure，并保留旧命令别名。状态栏显示 `declared/configured/ready/stale/unavailable`，不要把 build exit code 当作 CDB availability。

- [ ] **Step 7: 运行插件单元测试并提交**

在插件仓库运行：

```bash
npm test
git add src/ideProtocol.ts src/ideWorkflow.ts src/extension.ts src/workflow.ts src/cliController.ts src/analysis.ts package.json test/ideProtocol.test.ts test/ideWorkflow.test.ts
git commit -m "feat: consume mcpp IDE snapshots and early CDB events"
```

## 任务 9：真实 VS Code/LLVM 发布门槛

**Files:**
- Create: `/Users/cltx/projects/mcpp/mcpp-vscode/test/extension-host/ide-fixture/`
- Create: `/Users/cltx/projects/mcpp/mcpp-vscode/test/extension-host/ide-e2e.ts`
- Modify: `/Users/cltx/projects/mcpp/mcpp-vscode/package.json`
- Modify: `tests/e2e/198_ide_configure.sh`
- Modify: `tests/e2e/199_ide_prepare.sh`
- Create: `docs/ide/ide-client-contract.md`

- [ ] **Step 1: 建立真实 extension host runner**

使用 VS Code Extension Development Host、真实编译后的 mcpp binary 和显式匹配 LLVM/clangd。fake process 仅覆盖 parser/workflow 单测，不能作为发布证据。

- [ ] **Step 2: 验证提前 CDB**

fixture 源码包含错误模板、缺失符号和同名 `util.h`。在 configure event 后立即打开文件，断言 clangd 使用 snapshot CDB，诊断路径和 include 路径来自项目，不是 Apple SDK fallback。

- [ ] **Step 3: 验证模块两阶段行为**

先断言 configured 阶段普通文件补全/跳转可用，module import 显示 pending；prepare 完成后重新检查跨模块补全、definition/reference 和 PCM producer identity。

- [ ] **Step 4: 验证 UChat 和多根 workspace**

用 `/Users/cltx/ppp/UChat/server` 选择不同 member/target，验证 configurationId、CDB 和 generated sources 不串用。多根 A/B 切换时只让活动工程更新窗口级 clangd。

- [ ] **Step 5: 验证失败回退、信任和取消**

分别制造网络/依赖失败、build.mcpp 失败、module BMI 失败、取消和旧 mcpp。断言 last-known-good 保持、迟到事件被忽略、未受信任 workspace 不执行外部程序、legacy 状态可见。

- [ ] **Step 6: 运行完整发布验证**

运行：

```bash
mcpp test
npm test
npm run package
```

再在真实 VS Code/LLVM 矩阵运行 extension-host E2E，记录 clangd revision、compiler revision、CDB path、snapshotId 和 artifact state。

- [ ] **Step 7: 提交文档和发布门槛**

```bash
git add docs/ide/ide-client-contract.md tests/e2e/198_ide_configure.sh tests/e2e/199_ide_prepare.sh
git commit -m "docs(ide): document client contract and release gates"
```

## 任务 10：后续 BSP adapter 评估

**Files:**
- Create: `docs/ide/bsp-mapping.md`
- Create: `src/ide/bsp_adapter.cppm`（仅在有第二个真实客户端需求后）
- Create: `tests/unit/test_ide_bsp_mapping.cpp`（与 adapter 同步）

- [ ] **Step 1: 用 schema 1 建立映射表**

固定 `workspace -> workspace/buildTargets`、target/source/dependencies、test/run、diagnostics 和 C++ compile options/artifacts 的映射，标记 mcpp 特有字段如何放入 extension namespace。

- [ ] **Step 2: 只有真实客户端需求成立时才实现 JSON-RPC transport**

adapter 必须调用同一 `project_model/configure/prepare` API，不得在 BSP 层重新解析 manifest、CDB 或 toolchain。没有第二个客户端时只交付映射文档，不增加 daemon 维护成本。

- [ ] **Step 3: 运行 mapping tests 并记录兼容性**

schema 增加字段时先更新 mcpp snapshot contract，再更新 BSP adapter；不能让 BSP 的未批准变化反向破坏 mcpp schema。

## 交付顺序和停止条件

第一期交付范围是任务 1-7：mcpp 能在普通源码编译前发布 configured CDB，并能按最小 goals 准备模块产物。第二期是任务 8-9：VS Code 消费协议并通过真实 clangd/Extension Host。任务 10 延后到至少有一个非 VS Code 客户端或明确的 BSP 集成需求。

每期都必须满足：

- focused unit tests 通过；
- 真实 CLI E2E 通过；
- `git diff --check` 通过；
- 没有把历史 CDB 条目、单代表性 CDB 推断或人类 CLI 正则重新引入新主链路；
- 文档中的 capability、state、side-effect 和回退承诺与测试一致。
