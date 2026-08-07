# mcpp 通用 IDE 工程模型与提前 CDB 设计

**状态：** 提案，供实施前审阅

**日期：** 2026-08-06

## 1. 目标与结论

mcpp IDE 集成的目标不是让 VS Code 猜出更多 CDB 信息，而是让 mcpp 成为工程模型、配置解析和 IDE 构建产物的权威提供者。VS Code、clangd、测试面板、调试器和其他 IDE 客户端消费同一份版本化模型。

本方案采用三阶段生命周期：

```text
mcpp ide inspect
    -> 只读读取声明模型和已发布状态

mcpp ide configure
    -> 解析 workspace、依赖、feature、toolchain、build.mcpp 和 source
    -> BuildPlan 成功后立即发布新 CDB
    -> 不编译普通实现单元，不链接最终程序/库

mcpp ide prepare
    -> 复用 configure 结果
    -> 生成必要的 generated outputs、std BMI 和工程模块 BMI
    -> 发布 ready 快照
```

关键决策是：CDB 不需要等待最终编译成功。当前 Ninja 后端已经在启动 Ninja 之前由 `BuildPlan + CompileFlags` 写出 CDB（见 `src/build/ninja_backend.cppm` 的 `NinjaBackend::build`）。缺少的是独立的、可观察的 configure 生命周期，以及 CDB 的 provenance、freshness 和 snapshot 所有权。

普通 C++ 语法错误通常不会阻止当前默认的正则模块扫描，因此 configure 可以在源码编译前产出带有正确 compiler、include、macro、标准、sysroot 和 module search path 的 CDB。依赖解析、工具链解析、`build.mcpp`、generated files 或模块图验证失败时，mcpp 不伪造完整 CDB，而是保留 last-known-good 并返回结构化诊断。

## 2. 现状证据与问题边界

### 2.1 CDB 不是编译正确性的证明

clangd 需要的是每个文件的编译上下文，不是该文件已经成功编译。clangd 官方说明中，CDB 提供的是 compiler argv、工作目录、语言标准、宏和 include 路径；这些参数决定解析上下文，但不代表源文件无语法错误。

mcpp 当前的链路是：

```text
manifest/workspace/selector
  -> dependency/toolchain resolution
  -> generated_files 和 build.mcpp
  -> module scan/validate
  -> std BMI preparation
  -> BuildPlan
  -> Ninja emit + compile_commands.json
  -> Ninja execution
```

`src/build/compile_commands.cppm` 已经可以只根据 `BuildPlan + CompileFlags` 生成 CDB，不需要运行 Ninja。`src/build/ninja_backend.cppm` 先写 `build.ninja` 和 CDB，再执行 hermetic link preflight 和 Ninja。因此“只有成功 build 后才有 CDB”不是当前源码事实；实际问题是：

- 插件通过 `mcpp build` 任务间接触发 CDB 生成；
- 插件没有收到“配置已完成、CDB 已发布”的稳定事件；
- CDB 目前会合并历史条目，不能代表单一 configuration；
- CDB 没有绑定 `configurationId`、toolchain fingerprint、生成来源和 PCM 状态；
- `prepare_build()` 在 BuildPlan 之前可能安装依赖、运行 `build.mcpp`、物化 generated files、构建 host tools 或 std BMI；失败时没有独立的部分结果协议；
- 插件从一个代表性 CDB 条目反推整个工程和模块能力。

### 2.2 必须区分三个状态

| 状态 | 可用数据 | clangd 行为 | 产生条件 |
| --- | --- | --- | --- |
| `declared` | manifest、workspace、声明的 targets/features | 不执行 mcpp；可显示工程诊断 | `inspect` 成功 |
| `configured` | resolved model、toolchain、fresh CDB、预期 artifact 路径 | 立即可解析普通 C++；依赖模块可能报告 artifact pending | BuildPlan 和 CDB 发射成功 |
| `ready` | configured 数据加上 generated outputs、std/工程 BMI 和匹配指纹 | 允许跨模块补全、跳转和语义诊断 | `prepare` 的要求全部满足 |

`configured` 不是编译失败，也不是降级 CDB。它是一个明确的 IDE 可用状态。`ready` 只表示 IDE 所需的产物已满足，不表示最终链接或运行时测试成功。

## 3. 设计目标

### 3.1 必须实现

- mcpp 原生 workspace/member/target/profile/feature/dependency/toolchain 的机器可读模型。
- CDB 在 BuildPlan 完成后、普通源码编译前发布。
- CDB 与 configuration、toolchain、target triple、stdlib、C++ standard、PCM producer 和 generated files 绑定。
- CDB 使用 snapshot-local、无历史合并的内容；兼容旧客户端时再单独复制到项目根。
- configure 和 prepare 输出版本化 JSON/NDJSON，支持进度、诊断、取消和失败回退。
- 允许插件在 CDB 发布事件后立即配置 clangd，并在 ready 事件后刷新模块状态。
- 为其他 IDE、测试/调试工具和 CI 保留同一数据模型入口。
- 对 LLVM、GCC、MSVC、外部 CMake/CDB-only 项目明确 capability，而不是猜测完整模块支持。

### 3.2 不在第一版承诺

- 自研 C++ language server 或替代 clangd。
- 一开始实现常驻 daemon、LSP 或完整 JSON-RPC 服务。
- 让 configure 在无依赖、无 toolchain 或 `build.mcpp` 失败时猜出“正确”的完整 CDB。
- 承诺 GCC `.gcm`、MSVC `.ifc` 能被 clangd 读取。
- 解析所有 CMake/Meson/Bazel 工程的完整语义。外部工程先提供 CDB-only fallback。
- 把 Cargo、CMake 或 BSP 的内部对象直接作为 mcpp wire DTO。

## 4. 方案比较

### 4.1 方案 A：继续让插件监听根 CDB

这是当前行为的低成本延伸。插件在 `compile_commands.json` 出现或变化时重新配置 clangd，构建失败但 CDB 存在时保留配置。

优点是改动少，且当前 watcher 已能观察构建过程中的 CDB 写入。缺点是没有 configuration 选择、CDB provenance、依赖图、PCM ownership、取消和跨客户端契约；插件仍需从单个 CDB 条目和人类 CLI 输出推断工程。

结论：保留为旧 mcpp fallback，不作为通用架构。

### 4.2 方案 B：直接实现 BSP server

BSP 的目标是减少 IDE 与 build tool 的两两集成，提供 workspace/build targets/sources/dependencies/tasks，并以 JSON-RPC 管理生命周期。它与 LSP 可以组合，概念上适合 mcpp。

但 BSP 当前规范明确标注为未批准标准，核心结构仍可能变化；C++ 扩展需要额外协议，PCM freshness、CDB ownership、`build.mcpp` 副作用和 mcpp 特有 capability 仍需自定义。直接实现 BSP 会把传输、daemon 生命周期和工程模型同时引入，无法快速改善现有插件。

结论：不作为第一条 wire protocol；将公共模型设计成可映射到 BSP，未来按实际客户端需求提供 BSP adapter。

### 4.3 方案 C：mcpp 版本化快照 + one-shot CLI，未来适配 BSP（推荐）

核心协议是 mcpp 自有的稳定 JSON/NDJSON CLI，采用 Cargo metadata 的版本化/增量兼容原则和 CMake File API 的 query/reply、版本化 object kind、不可变回复文件思路。VS Code 先消费 CLI；需要时再由一个薄适配器映射到 BSP。

优点是直接复用现有 `mcpp` 进程模型、测试体系、BuildPlan 和 CDB 生成逻辑，能先解决 clangd，再逐步接入测试/运行/调试；缺点是 mcpp 需要维护自己的 schema，并且未来 BSP 适配需要额外映射。

结论：采用方案 C。

## 5. 命令和生命周期契约

### 5.1 命令

```text
mcpp ide inspect --format json [selectors]
mcpp ide configure --format ndjson [selectors]
mcpp ide prepare --format ndjson [--configuration-id <id>] [selectors]
```

当前未发布的 `mcpp ide snapshot --format json` 保留为 `inspect` 的兼容别名，输出中将 `kind` 标为 `mcpp.ide.inspection`，避免把声明检查误称为已发布构建快照。

selectors 与现有 build/test 保持一致：`--package`、`--workspace`、`--profile`、`--target`、`--features`、`--cap`、`--include-dev-dependencies`。解析后的 selector 必须在 wire 中回显。

### 5.2 `inspect`

`inspect` 只读读取 manifest、workspace members、声明 target、已发布 snapshot index 和现有 CDB 状态。它不得初始化 `MCPP_HOME`、刷新 index、安装依赖、运行工具、编译 `build.mcpp` 或写项目文件。

它可以输出声明模型和 last-known-good 的摘要，但不能把现有根 CDB 直接标为 `ready`。没有 mcpp 发布 metadata 的根 CDB 状态为 `legacy/unknown`。

### 5.3 `configure`

`configure` 是显式授权的配置操作，允许：

- 解析和安装所需依赖、toolchain；
- 物化 `[generated_files]`；
- 编译/运行 `build.mcpp` 和必要的 host tools；
- 扫描并验证 module graph；
- 构造 resolved model、toolchain identity、BuildPlan 和 CompileFlags；
- 在任何普通项目 TU 或最终链接开始前发布 CDB。

`configure` 不请求普通实现 `.o`、最终库或最终程序。若当前 toolchain 的 `import std` 需要预编译 BMI，configure 只记录可确定的预期路径和 `pending` 状态；std/工程 BMI 的实际物化属于 `prepare`。如现有 `ensure_built()` 同时负责路径计算和编译，先提取纯路径/metadata 函数，再保留 `ensure_built()` 作为 prepare 阶段实现。

configure 的成功条件是“resolved BuildPlan 和 snapshot-local CDB 发射并发布成功”，不是源码编译成功。

### 5.4 `prepare`

`prepare` 复用或重新执行 configure，然后只执行 IDE 必需的 goals：

- generated action/source outputs；
- std/std.compat BMI；
- 提供模块的 CompileUnit 对应 BMI；
- 模块接口编译器无法分离出的接口对象。

它不请求普通实现 TU 的对象，也不请求 `LinkUnit::output`。host helper、`build.mcpp` 和请求的 host tool 可能仍需编译、链接和运行；这些副作用必须在事件和 manifest 中明确列出。

## 6. Wire model

### 6.1 顶层对象

```json
{
  "schemaVersion": 1,
  "kind": "mcpp.ide.snapshot",
  "projectId": "path-sha256:...",
  "configurationId": "config-sha256:...",
  "snapshotId": "snapshot-sha256:...",
  "phase": "configured",
  "state": "configured",
  "mcpp": {
    "version": "2026.8.5.2",
    "protocol": {"min": 1, "max": 1},
    "capabilities": ["workspace-model", "compile-commands", "module-artifacts"]
  },
  "request": {"selectors": {}, "mode": "configure"},
  "workspace": {},
  "configurations": [],
  "targets": [],
  "dependencies": [],
  "toolchain": {},
  "artifacts": {},
  "diagnostics": []
}
```

客户端必须忽略未知字段。`schemaVersion` 只在不兼容字段或语义变化时增加；新增字段和新增枚举值遵循 Cargo metadata 的兼容规则。路径必须是绝对路径，wire 中使用 `/` 分隔符。

### 6.2 ID 和 freshness

- `projectId`：由物理 workspace root 的规范化绝对路径计算，不能把 manifest 内容变化混入，以保持同一工程的 UI 状态。
- `configurationId`：由 selector、effective profile、target triple、feature 集、offline/cache 模式和显式 toolchain 选择的规范化值计算。它区分配置，但不代表该配置当前已解析成功。
- `snapshotId`：由 schema、resolved model、toolchain identity、CDB 内容摘要、module graph、artifact metadata 和输入指纹计算。它随 manifest、lockfile、toolchain、CDB 或产物变化。
- 工具链 fingerprint 继续复用 `src/toolchain/fingerprint.cppm` 的 BMI safety 语义，但 wire 另列 compiler、driver identity、target triple、stdlib、standard 和来源。

### 6.3 工程和配置对象

`workspace` 描述 root manifest、members 和 member selection。`targets` 为每个 target 提供稳定 ID、member、kind、sources、generated sources、entry main、test/run/debug capability。`dependencies` 描述 resolved package identity、source/provenance、feature activation 和有向 edge；不能从 CDB 路径推断。

`toolchain` 描述 compiler family/path/version/driver identity、clangd compatibility、target triple、stdlib/sysroot、module dialect 和来源。插件不再从 `mcpp toolchain list` 人类输出正则推断这些字段。

### 6.4 Artifacts

每个 artifact 包含：

```json
{
  "kind": "compile-commands",
  "owner": "target-id",
  "path": "/project/.mcpp/ide/replies/compile_commands-<hash>.json",
  "state": "ready",
  "producer": "mcpp",
  "fingerprint": "...",
  "inputs": ["configurationId", "toolchainFingerprint"],
  "lastKnownGood": false
}
```

artifact 状态包括 `pending`、`ready`、`stale`、`missing`、`unavailable`。CDB 必须由当前 resolved plan 单独发射，禁止复用 `write_compile_commands()` 的历史 merge 语义作为权威 IDE CDB。兼容旧 clangd 的根 `compile_commands.json` 是一个原子复制的 compatibility projection，不是 snapshot source of truth。

### 6.5 NDJSON 事件

`configure` 和 `prepare` 的 stdout 每行只输出一条 JSON，格式如下：

```json
{"schemaVersion":1,"seq":1,"type":"operation-started","operationId":"...","operation":"configure"}
{"schemaVersion":1,"seq":2,"type":"progress","operationId":"...","phase":"resolve","completed":2,"total":5}
{"schemaVersion":1,"seq":3,"type":"diagnostic","operationId":"...","diagnostic":{}}
{"schemaVersion":1,"seq":4,"type":"snapshot-published","operationId":"...","phase":"configured","snapshotId":"...","manifest":"...","compileCommands":"..."}
{"schemaVersion":1,"seq":5,"type":"operation-finished","operationId":"...","operation":"configure","status":"success","phase":"configured"}
```

事件包含单调递增 `seq`。失败、取消和超时也必须有 `operation-finished`，并携带 `status`、`diagnosticCodes` 和 `lastKnownGoodSnapshotId`。未受信任 workspace 不执行这些命令。

## 7. Snapshot 发布和早期 CDB

### 7.1 发布目录

使用 mcpp 自己管理的目录，不依赖项目根写入：

```text
<work-root>/.mcpp/ide/
  configurations/<configurationId>/
    current.json
    last-known-good.json
  replies/
    index-<generation>.json
    snapshot-<snapshotId>.json
    compile_commands-<cdbHash>.json
  artifacts/<toolchainFingerprint>/
    std/
    modules/
    generated/
```

回复文件写入临时文件后关闭并校验，再使用同目录 rename 发布。`index-<generation>.json` 只引用完整回复文件；客户端先读 index，再按引用读取对象。这借鉴 CMake File API 的 reply index 和不可变 reply 文件，避免读到半个 JSON。

### 7.2 CDB 发布时序

```text
resolve inputs
  -> BuildPlan
  -> compute_flags
  -> emit fresh CDB to replies/compile_commands-<hash>.json
  -> validate JSON entries and paths
  -> publish configured snapshot/index
  -> emit snapshot-published
  -> optional prepare goals
```

插件收到 `snapshot-published(phase=configured)` 后立即将 clangd 的 compile-commands directory 指向该 CDB 所在目录，或使用 `--compile-commands-dir` 指向 compatibility projection。此时普通文件的语义解析不依赖最终 build 成功；import module 的文件可以暂时显示 `pending`，直到 prepare 发布 ready snapshot。

### 7.3 回退

- configure 失败且存在 last-known-good：不替换当前 clangd 数据；发布失败事件，返回 last-known-good ID。
- configure 失败且没有 last-known-good：插件保持语法高亮，显示结构化错误，不执行 CDB 猜测。
- prepare 失败：configured snapshot 仍然可用；last-known-good 只有在全部要求满足时才更新。
- 取消或进程崩溃：不更新 index，不把半成品标为 ready；临时目录由下一次操作清理。

项目目录中的 `generated_files` 和任意 `build.mcpp` 代码执行无法纳入 mcpp 事务回滚。协议只能保证 mcpp 自己管理的 snapshot/reply/index 原子发布，并在诊断中准确区分“项目生成文件已写入”和“IDE 快照未发布”。

## 8. 后端和核心模块边界

需要从当前内部模型提取以下公共边界，而不是序列化 `BuildPlan`：

| 模块 | 职责 |
| --- | --- |
| `mcpp.build.resolution` | 返回 resolved packages、edges、features、provenance、effective toolchain 以及 generated/action 结果 |
| `mcpp.build.artifact_layout` | 统一计算 object、BMI、std、generated 和 action 输出路径及 IDE goals |
| `mcpp.build.compile_commands` | 提供 fresh-only CDB 发射；保留历史 merge 仅给旧 build/test 工作流 |
| `mcpp.build.backend` | 分离 emit build files 与 execute goals；IDE goals 不触发 link preflight |
| `mcpp.ide.model` | 稳定 wire DTO，不导入 build-plan 内部类型 |
| `mcpp.ide.inspect` | M0 只读声明和已发布状态读取 |
| `mcpp.ide.configure` | 运行解析、生成 CDB 和 configured snapshot |
| `mcpp.ide.prepare` | 运行最小 artifact goals、校验并发布 ready snapshot |
| `mcpp.ide.events` | 统一 NDJSON envelope、进度、诊断、取消和完成状态 |
| `mcpp.ide.publish` | 临时文件、摘要校验、index/current/last-known-good 原子发布 |

`prepare_build()` 当前将 `packages` 和 `DependencyEdge` 保留为局部类型，返回的 `BuildContext` 丢失了完整依赖图。第一阶段应提取 resolution DTO 或返回对象；不得在插件中复刻这部分逻辑。

## 9. 客户端边界

VS Code 侧保留：Workspace Trust、QuickPick/member/target/profile 选择、Task/进度/取消、clangd 扩展配置和重启、状态栏、输出频道、watcher、singleflight、迟到事件抑制、旧 mcpp CDB fallback。

VS Code 侧删除或降级：单代表性 CDB 条目推断全工程 capability、路径猜测 package/target、正则解析 `mcpp toolchain list`。这些字段从 snapshot 的 `targets`、`toolchain`、`artifacts` 和 `diagnostics` 获取。

首次打开受信任工程时：

1. `inspect` 显示声明和 last-known-good 状态；
2. 若无当前 configured snapshot，启动 `configure`；
3. 收到 configured CDB 事件后立即配置 clangd；
4. 后台启动或继续 `prepare`；
5. ready 事件到达后仅刷新需要的 module arguments/status；
6. 失败时保留当前可用 CDB，并显示可操作诊断。

多根 workspace 按 `projectId + configurationId` 隔离状态；由于官方 clangd 扩展通常是窗口级配置，只有活动工程更新窗口级 clangd，后台工程只维护快照和状态。

## 10. 通用协议对照

### Cargo metadata

Cargo 的可借鉴点是 `cargo metadata --format-version 1`：它输出 workspace members、packages、targets、features、resolved dependency graph、target directory 和 schema version；同一 format version 允许新增字段和 enum 值，消费者不依赖 opaque ID 的内部表示。mcpp 应复制这些兼容原则，但把 C++ toolchain、compile units、modules 和 artifact freshness 纳入自己的模型。

Cargo 的 `cargo check --message-format=json` 对应 mcpp 的 configure/prepare 事件：它让 IDE 获得结构化诊断和进度，而不是解析人类日志。Cargo 的统一 rustc/toolchain 环境比 C++ 简单，mcpp 必须以 capability 和 provenance 表达 GCC/MSVC/Clang 差异。

### CMake File API

CMake File API 的 query/reply、object kind 独立版本、reply index、不可变回复文件和客户端私有查询目录很适合 mcpp 的 snapshot 发布。mcpp 不应复制 CMake 的 schema，而应采用同样的“客户端先读 index，再跟随引用”的一致性策略。

### Build Server Protocol

BSP 的 build server/IDE client 边界和 JSON-RPC 生命周期值得作为未来 adapter 的目标。BSP 当前规范仍明确标注为未批准标准，C++ 扩展还需要自定义 compile options 和 artifact 能力，因此第一版不把 BSP 作为 mcpp 核心 wire。公共 mcpp model 的 workspace、target、sources、dependencies、test/run 能力应保持可映射；需要外部 BSP 客户端时增加独立 `mcpp ide bsp` adapter，而不是改变 snapshot schema。

## 11. 错误、信任和安全

稳定诊断必须包含 `code`、severity、message、source、path/range、phase 和可选 hint。`mcpp.diag::Record` 可作为来源，但不能直接作为 wire DTO，因为它当前没有稳定 code/path/range 且会立即渲染。

configure/prepare 运行任意项目代码和外部工具，只有 workspace trusted 才允许。所有外部 argv 使用结构化数组，不拼 shell 字符串。artifact 路径必须限制在 mcpp 管理的 work root、toolchain store 或明确的 dependency root；manifest 成员路径继续执行 workspace containment 和 symlink 策略。

取消需要 operation ID、generation token 和进程组/Job Object 语义。当前 `capture_exec_deadline` 的 POSIX `SIGKILL` 和 Windows best-effort deadline 不能直接宣称支持真实取消；第一阶段先实现插件杀进程树后的迟到事件丢弃，再补平台一致的 process cancellation。

## 12. 兼容和迁移

- 新 mcpp：提供 `inspect/configure/prepare` 和 schema 1；`snapshot` 是兼容 alias。
- 旧 mcpp：插件探测不到 `mcpp.ide` capability 时，回退到 nearest manifest + root CDB，并在状态中标记 `legacy/partial`。
- 旧插件：如果只会读根 CDB，mcpp 可在选定默认 configuration 下原子生成根 `compile_commands.json` compatibility projection。
- 外部 CDB-only 工程：插件继续支持 clangd 原生行为；mcpp 不声称提供 workspace model 或 modules capability。
- schema 的协议版本、对象 kind 版本、capability 字符串和诊断 code 必须在 mcpp 与插件测试中共同锁定。

## 13. 验收标准

### mcpp 核心

1. `configure` 在普通源文件包含明显语法错误时仍能发布有效 CDB；CDB 的 compiler、directory、include、macro、standard 和 sysroot 与 BuildPlan 相同。
2. CDB 发布事件出现在任何普通项目 TU 的 Ninja compile edge 之前。
3. configure 不产生普通实现对象，也不链接最终 binary/library。
4. `prepare` 只生成声明的 generated outputs、std/工程 BMI 和不可分离的模块接口对象；不生成普通实现对象或最终链接产物。
5. CDB 是 fresh-only；不同 configuration 不会通过历史 merge 互相污染。
6. 修改 manifest、lock、selector、toolchain 或 source fingerprint 后旧 snapshot 为 stale；失败不覆盖 last-known-good。
7. 取消、超时、权限拒绝、依赖失败和 build.mcpp 失败分别有稳定诊断和结束事件。

### VS Code 与真实 clangd

1. 无 CDB 工程打开后，configure 发布事件会在完整 build 结束前配置 clangd。
2. UChat 多 member/target 选择能让 tests 中的同名 `util.h` 解析到项目 CDB，而不是 macOS SDK。
3. LLVM 22 CDB 不会因为 PATH 中存在 Apple Clang 而被插件选为 compiler/clangd identity。
4. configured 阶段普通文件可以补全、悬停和跳转；ready 阶段模块 import 的补全和跨模块跳转可用。
5. configure/prepare 失败时仍保留可用的 last-known-good clangd 状态。
6. 多根 workspace、未受信任 workspace、取消和迟到事件都有测试覆盖。

### 证据边界

Node 单元测试和 `clangd --check` 不能替代真实 Extension Development Host。发布前必须用真实 VS Code、真实 mcpp binary、匹配 LLVM/clangd、模块工程、build.mcpp、测试源码和多根 workspace 运行 E2E。

### 当前实现的验证边界

截至 2026-08-06，早期 CDB 实现还有两个 IDE 路径的测试缺口，不能据此推断核心 Ninja 构建后端存在回归：

1. 尚未通过真实 `configure_project()` 流程制造 staging 失败，并断言已有根 CDB 保持不变。普通 `mcpp build/test` 继续使用既有 `write_compile_commands()` 和 Ninja 执行路径，不经过 IDE 的 fresh CDB 发布与回退逻辑。完整 last-known-good 状态仍属于后续 snapshot publish 层，不应把它与当前测试缺口混为已实现能力。
2. cached dependency BMI 的发布前 staging 已有 helper 单测和 macOS/Clang E2E，但尚缺真实 GCC `.gcm`、MSVC `.ifc` 缓存命中工程。核心 Ninja 后端仍通过自己的 `stage_file` edges 物化缓存产物；此缺口的直接风险是 IDE CDB 引用了未物化或路径漂移的 BMI，而不是普通构建无法完成。

第二项仍跨越 `BuildPlan::cachedBmi`、`bmi_traits()`、`stage_file()` 和 Ninja BMI 命名等共享契约。应按任务 3 将目标路径收敛到 `mcpp.build.artifact_layout`，由 Ninja 与 IDE 共同消费，再补真实工具链矩阵。GCC/MSVC E2E 只证明路径和产物契约，不代表 clangd 能消费 `.gcm` 或 `.ifc`；该能力仍按工具链 capability 明确表达。

当前 `configure` 已在解析前发送带 `operationId` 的 `operation-started`，并在成功或
失败的全部后续事件中沿用该 ID；失败序列为
`diagnostic -> operation-finished(status=failed)`。解析前尚无 resolved
`configurationId`，因此早期 started 事件不携带该字段，客户端应以 `operationId`
关联整个生命周期。阶段化 `progress`、取消、迟到事件过滤，以及
index/current/last-known-good 发布仍属于后续 publish/prepare 任务。现阶段不能据此
宣称完整的长任务管理和失败回退协议已经完成。

当前 `configured_snapshot_id` 已绑定 configuration、BuildContext fingerprint 和
CDB 内容；完整 resolved module graph、artifact metadata 和输入 provenance 尚未
进入 snapshot DTO/ID，需随后续模型与 publish 层一并补齐。
