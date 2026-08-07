# 11 — IDE 协议

本文描述当前 `mcpp` 二进制已经实现的 IDE 协议，面向 IDE 插件、语言工具
启动器及其他机器客户端。协议输出与面向人的 CLI 输出分离；客户端应请求并
解析下面的格式，不要抓取进度文本。

## 1. 范围与兼容性

当前公开的命令只有：

```text
mcpp ide snapshot [selectors]
mcpp ide configure [selectors]
```

两个命令当前都使用 schema 版本 `1`。对于 snapshot 响应，客户端应先检查
`schemaVersion`、`kind` 和 `mcpp.protocol` 范围；对于 configure 事件，应验证
`schemaVersion`、`type`、`seq` 和 `operationId`。未知字段必须忽略。
`events`、`model`、`inspect`、`publish` 是内部 C++ module，不是 CLI 子命令。设计中的 `prepare`、
配置索引、取消和进度协议尚未实现。

命令从当前目录向父目录查找 `mcpp.toml`，目前不接受项目路径参数。

## 2. Selector

两个命令都支持以下选项：

| 选项 | 含义 |
| --- | --- |
| `-p, --package MEMBER` | 按工作空间相对路径或目录 basename 选择一个成员。 |
| `--workspace` | `snapshot` 检查全部成员；`configure` 当前拒绝。 |
| `--profile NAME` | 请求构建 profile。 |
| `--target TRIPLE` | 请求 target triple。 |
| `--features LIST` | 逗号分隔的 feature selector。 |
| `--cap LIST` | 逗号分隔的 capability provider pin。 |
| `--include-dev-dependencies` | 显式纳入开发依赖。 |
| `--format FORMAT` | `snapshot` 只接受 `json`；`configure` 只接受 `ndjson`。 |

CSV selector 会丢弃空项，但不会去除空格。当前没有 `--offline` selector。

`configure` 发现 `tests/**/*.cpp` 后，即使没有显式指定
`--include-dev-dependencies`，也会自动启用开发依赖，保证测试 TU 使用与真实
测试构建一致的 include 路径和宏定义。

## 3. 只读 snapshot

`mcpp ide snapshot` 默认使用 `--format json`，不会解析依赖、安装依赖、写项目
文件，也不会发布 CDB。`partial`、`stale`、`configured` 状态返回 `0`；不可用
状态返回 `3`。

顶层结构如下：

```json
{
  "schemaVersion": 1,
  "kind": "mcpp.ide.snapshot",
  "snapshotId": "fnv1a64:...",
  "state": "partial",
  "mcpp": {
    "version": "2026.8.7.1",
    "protocol": {"min": 1, "max": 1},
    "capabilities": ["workspace-inspection", "manifest-diagnostics", "compile-commands-location"]
  },
  "request": {"root": ".", "mode": "read-only", "selectors": {}},
  "workspace": {"root": ".", "manifest": "...", "members": [], "selectedMembers": []},
  "artifacts": {"state": "partial", "compileCommands": []},
  "diagnostics": []
}
```

`workspace.members[]` 描述已经成功解析的包，包含 `name`、`version`、
`workspacePath`、`root`、`manifest` 和 `targets[]`。target 包含 `name`、
`kind`（`library`、`shared-library`、`binary` 或 `test-binary`），以及可选
的 `main`。

`artifacts.compileCommands[]` 包含 `member`、`path`、`state`（`missing`、
`configured` 或 `stale`），以及可选的 `snapshotId` 和 `configurationId`。

状态聚合规则为：

```text
缺少 artifact -> partial
存在但未经验证的根 CDB -> stale
configured metadata 有效且 reply CDB 是仍存在的普通文件 -> configured
```

`ready` 只是为未来“artifact 已准备好”状态预留，当前不会生成。

诊断包含 `code`、`severity`、`message`、`source: "mcpp"`，并可包含 `path` 和
带 `line`、`column` 的一基 `range`。已知 code 包括：
`MCPP_IDE_MANIFEST_NOT_FOUND`、`MCPP_IDE_MANIFEST_INVALID`、
`MCPP_IDE_MEMBER_MANIFEST_MISSING`、`MCPP_IDE_MEMBER_MANIFEST_INVALID`、
`MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND`、`MCPP_IDE_ARTIFACTS_MISSING`、
`MCPP_IDE_ARTIFACTS_UNVERIFIED`、`MCPP_IDE_ARTIFACTS_UNAVAILABLE`、
`MCPP_IDE_SNAPSHOT_INVALID`、`MCPP_IDE_SNAPSHOT_STALE` 和
`MCPP_IDE_UNSUPPORTED_FORMAT`。

只读检查会验证 metadata、项目根路径约束，以及 CDB 路径仍为普通文件；它不会
打开或解析 CDB，也不会重新计算 manifest、lockfile、源码集合、selector 或工具链
fingerprint。因此 `configured` 只表示上一次 metadata 仍指向现存普通文件，不表示
CDB 内容有效或当前输入已经重新验证。插件可以在交给语言服务前自行验证 JSON；
输入变化后应再次运行 `configure`。

## 4. Configure 与 NDJSON 事件

`mcpp ide configure` 默认使用 `--format ndjson`，stdout 的每一行都是一个独立
JSON 对象。成功顺序固定为：

```text
operation-started
snapshot-published
operation-finished (success)
```

失败顺序为：

```text
operation-started
diagnostic
operation-finished (failed)
```

所有事件都包含以下 envelope：

```json
{
  "schemaVersion": 1,
  "seq": 2,
  "type": "snapshot-published",
  "operationId": "operation-fnv1a64:..."
}
```

`seq` 从 `1` 开始严格递增；同一次调用的所有事件共享 `operationId`。

`operation-started` 包含 `operation: "configure"`。`snapshot-published` 包含
`phase`、`state`、`projectId`、`configurationId`、`snapshotId`、内容寻址的
`compileCommands` 路径、兼容 CDB 路径、`compileCommandCount`、`toolchain` 和
`toolchainFingerprint`，还可能包含：

```json
"stdModule": {
  "kind": "std-module",
  "path": "...",
  "state": "ready"
}
```

成功的 `operation-finished` 包含 `status: "success"`、`operation`、`phase` 和
`configurationId`。失败会先发出 code 为 `MCPP_IDE_CONFIGURE_FAILED` 的诊断，
再发出 `status: "failed"` 和 `diagnosticCodes`。

Configure 成功发布返回 `0`；格式错误在 operation 开始前返回 `2`；manifest、
解析、selector、工具、stage、发布或未预期异常返回 `3`。取消、超时和 `130`
退出语义尚未实现。

## 5. ID 与发布文件

三类 ID 的范围不同：

- `projectId`：物理工作空间根目录。
- `configurationId`：规范化 selector、解析后的 profile/target、cache mode、
  language standard 和工具链 fingerprint。
- `snapshotId`：configured CDB 发布及其 provenance。只读 snapshot 顶层的
  `fnv1a64:*` 是 inspection 文档 ID，不是 configured `snapshot-*` ID。

成功 configure 后相关文件为：

```text
<project>/.mcpp/ide/replies/compile_commands-<hash>.json
<project>/.mcpp/ide/replies/snapshot-<hash>.json
<project>/.mcpp/ide/current.json
<project>/compile_commands.json
<project>/.mcpp/ide/.lock
```

reply CDB 是协议的事实来源；根目录 CDB 是供既有 clangd 使用的兼容投影。
`current.json` 记录 `schemaVersion`、`kind: "mcpp.ide.configured-snapshot"`、
phase、各 ID、项目根、两个 CDB 路径、命令数和工具链身份。

每个 JSON 文件都通过临时文件和替换写出。根 CDB 与 `current.json` 是两个独立
替换点，不是一个跨文件操作系统事务。普通发布错误会尝试恢复旧根 CDB；如果
进程恰好在两个替换点之间崩溃，仍可能短暂留下新 CDB 与旧 metadata 不一致。

## 6. Configure 做什么

Configure 从解析后的 `BuildPlan` 和 compile flags 生成 CDB，不编译普通项目 TU，
也不链接最终可执行文件，但它不是只读操作：解析过程可能安装依赖或工具链、执行
根项目及依赖的 `build.mcpp` 代码、更新 `mcpp.lock`、写入 target resolution
metadata、创建或更新缓存、发现测试，以及 stage 标准库或已命中的依赖 BMI。
IDE 对不可信工程运行 configure 前必须取得 workspace trust 或等价的明确授权；
未授权时只能运行只读的 `snapshot`。

CDB 包含普通源码和发现到的测试源码；发布前会 stage 已缓存的模块前置产物。未
命中的工程或依赖模块 BMI 不会由此命令完整生成，因此模块补全可能要等普通 build
或未来的 prepare 命令。stage GCC `.gcm` 或 MSVC `.ifc` 不代表 clangd 能消费这些
工具链的模块格式。

`mcpp ide configure --workspace` 当前明确拒绝。请使用 `--package`，逐个成员
执行 configure。

## 7. 客户端生命周期

以下行为属于 IDE 插件职责，不需要继续增加 mcpp 协议命令：

1. 先运行 `snapshot --format json`，发现成员和已有 artifact。
2. 工作空间使用 `workspace.members[].workspacePath`，逐成员运行
   `configure --package <workspacePath>`；不要把 `--workspace` 传给 configure。
   同一成员内串行执行，不同成员最多使用有限并发。
3. 根/成员 `mcpp.toml`、`mcpp.lock`、`build.mcpp`、源码集合或 module 声明、
   profile/target/features/capabilities selector、工具链选择发生变化时重新 configure。
   文件事件应 debounce；插件执行完 mcpp 依赖或工具链命令后也应重新 configure。
   不要因当前 configure 自己写出的 `mcpp.lock`、target metadata 或缓存事件立即再次
   触发 configure。不改变源码集合或 module graph 的普通源码内容编辑不需要重建 CDB。
4. 校验 `seq`，用 `operationId` 关联事件，忽略已经被新操作取代的事件。协议尚无
   cancellation，因此插件不应为同一成员启动重叠的 configure。
5. 把 `snapshot-published` 作为发布边界。reply CDB 是协议事实来源；如果语言服务
   必须读取名为 `compile_commands.json` 的文件，可以使用
   `compatibilityCompileCommands`，但必须等同一次 operation 的该事件到达后再启用。
   启动或崩溃恢复时应重新 configure，不能仅因根 CDB 存在就信任它。
6. configure 失败时继续使用此前可用的 clangd 配置，展示结构化诊断并标为 stale。
   schema 1 将 configure 失败统一报告为 `MCPP_IDE_CONFIGURE_FAILED`；插件不能解析
   面向人的 message 来推断锁竞争或 I/O 分类。同一成员的操作应串行，其他失败由
   用户主动重试。
7. 不要把 `configured` 当成 module 已完全 ready。先让 clangd 分析可用 TU；缺少
   BMI 时显示 module pending，并在用户要求完整 module 语义时提供普通
   `mcpp build`（仅测试准备可使用 `mcpp test`），命令完成后重新 configure。
   工作空间发现阶段不要静默启动完整构建。
8. 将 stdout 视为协议专用通道；人的诊断可能写入 stderr，不要把它当成事件解析。

当前协议是一次性命令。daemon、取消、工作空间扇出、主动 freshness 重算、ready
artifact snapshot 和 last-known-good 索引都属于后续扩展。
